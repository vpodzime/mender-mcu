/**
 * @file      main.c
 * @brief     Test application used to perform SonarCloud analysis
 *
 * Copyright joelguittet and mender-mcu-client contributors
 * Copyright Northern.tech AS
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <getopt.h>
#include <pthread.h>
#include <regex.h>
#include <signal.h>
#include <stdio.h>
#include "mender-client.h"
#include "mender-configure.h"
#include "mender-flash.h"
#include "mender-inventory.h"
#include "mender-log.h"
#include "mender-troubleshoot.h"

/**
 * @brief Mender client options
 */
static const struct option mender_client_options[] = { { "help", 0, NULL, 'h' },
                                                       { "mac_address", 1, NULL, 'm' },
                                                       { "artifact_name", 1, NULL, 'a' },
                                                       { "device_type", 1, NULL, 'd' },
                                                       { "tenant_token", 1, NULL, 't' },
                                                       { "private_key", 1, NULL, 'p' },
                                                       { NULL, 0, NULL, 0 } };

/**
 * @brief Mender client identity
 */
static mender_identity_t mender_identity = { .name = "mac", .value = NULL };

/**
 * @brief Private key path
 */
static char *key_path = NULL;

/**
 * @brief Mender client events
 */
static pthread_mutex_t mender_client_events_mutex;
static pthread_cond_t  mender_client_events_cond;

/**
 * @brief Network connnect callback
 * @return MENDER_OK if network is connected following the request, error code otherwise
 */
static mender_err_t
network_connect_cb(void) {

    mender_log_info("Mender client connect network");

    /* This callback can be used to configure network connection */
    /* Note that the application can connect the network before if required */
    /* This callback only indicates the mender-client requests network access now */
    /* Nothing to do in this test application just return network is available */
    return MENDER_OK;
}

/**
 * @brief Network release callback
 * @return MENDER_OK if network is released following the request, error code otherwise
 */
static mender_err_t
network_release_cb(void) {

    mender_log_info("Mender client released network");

    /* This callback can be used to release network connection */
    /* Note that the application can keep network activated if required */
    /* This callback only indicates the mender-client doesn't request network access now */
    /* Nothing to do in this test application just return network is released */
    return MENDER_OK;
}

/**
 * @brief Authentication success callback
 * @return MENDER_OK if application is marked valid and success deployment status should be reported to the server, error code otherwise
 */
static mender_err_t
authentication_success_cb(void) {

    mender_err_t ret;

    mender_log_info("Mender client authenticated");

#ifdef CONFIG_MENDER_CLIENT_ADD_ON_TROUBLESHOOT
    /* Activate troubleshoot add-on (deactivated by default) */
    if (MENDER_OK != (ret = mender_troubleshoot_activate())) {
        mender_log_error("Unable to activate troubleshoot add-on");
        return ret;
    }
#endif /* CONFIG_MENDER_CLIENT_ADD_ON_TROUBLESHOOT */

    /* Validate the image if it is still pending */
    /* Note it is possible to do multiple diagnosic tests before validating the image */
    if (MENDER_OK != (ret = mender_flash_confirm_image())) {
        mender_log_error("Unable to validate the image");
        return ret;
    }

    return ret;
}

/**
 * @brief Authentication failure callback
 * @return MENDER_OK if nothing to do, error code if the mender client should restart the application
 */
static mender_err_t
authentication_failure_cb(void) {

    /* Check if confirmation of the image is still pending */
    if (true == mender_flash_is_image_confirmed()) {
        mender_log_info("Mender client authentication failed");
        return MENDER_OK;
    }
    mender_log_error("Mender client authentication failed");

    /* Restart the application after authentication failure with the mender-server */
    /* The image has not been confirmed and the system will now rollback to the previous working image */
    /* Note it is possible to customize this depending of the wanted behavior */
    return MENDER_FAIL;
}

/**
 * @brief Deployment status callback
 * @param status Deployment status value
 * @param desc Deployment status description as string
 * @return MENDER_OK if the function succeeds, error code otherwise
 */
static mender_err_t
deployment_status_cb(mender_deployment_status_t status, char *desc) {

    /* We can do something else if required */
    mender_log_info("Deployment status is '%s'", desc);

    return MENDER_OK;
}

/**
 * @brief Restart callback
 * @return MENDER_OK if the function succeeds, error code otherwise
 */
static mender_err_t
restart_cb(void) {

    /* Application is responsible to shutdown and restart the system now */
    pthread_mutex_lock(&mender_client_events_mutex);
    pthread_cond_signal(&mender_client_events_cond);
    pthread_mutex_unlock(&mender_client_events_mutex);

    return MENDER_OK;
}

/**
 * @brief Get identity callback
 * @return MENDER_OK if the function succeeds, error code otherwise
 */
static mender_err_t
get_identity_cb(mender_identity_t **identity) {
    if (NULL != identity) {
        *identity = &mender_identity;
        return MENDER_OK;
    }
    return MENDER_FAIL;
}

/**
 * @brief Get user-provided keys callback
 * @return MENDER_OK if the function succeeds, error code otherwise
 */
static mender_err_t
get_user_provided_keys_cb(char **user_provided_key, size_t *user_provided_key_length) {
    if (NULL == key_path) {
        return MENDER_OK;
    }
    mender_log_info("Using key: `%s`", key_path);
    FILE *file = fopen(key_path, "r");
    if (NULL == file) {
        mender_log_error("Unable to open file");
        return MENDER_FAIL;
    }
    fseek(file, 0, SEEK_END);
    size_t length = ftell(file);
    fseek(file, 0, SEEK_SET);
    *user_provided_key = (char *)malloc(length + 1);
    if (NULL == *user_provided_key) {
        mender_log_error("Unable to allocate memory");
        fclose(file);
        return MENDER_FAIL;
    }
    int ret = fread(*user_provided_key, 1, length, file);
    if (ret != length) {
        mender_log_error("Unable to read file");
        fclose(file);
        return MENDER_FAIL;
    }
    fclose(file);

    *user_provided_key_length = length + 1;

    return MENDER_OK;
}

#ifdef CONFIG_MENDER_CLIENT_ADD_ON_CONFIGURE
#ifndef CONFIG_MENDER_CLIENT_CONFIGURE_STORAGE

/**
 * @brief Device configuration updated
 * @param configuration Device configuration
 * @return MENDER_OK if the function succeeds, error code otherwise
 */
static mender_err_t
config_updated_cb(mender_keystore_t *configuration) {

    /* Application can use the new device configuration now */
    if (NULL != configuration) {
        size_t index = 0;
        mender_log_info("Device configuration received from the server");
        while ((NULL != configuration[index].name) && (NULL != configuration[index].value)) {
            mender_log_info("Key=%s, value=%s", configuration[index].name, configuration[index].value);
            index++;
        }
    }

    return MENDER_OK;
}

#endif /* CONFIG_MENDER_CLIENT_CONFIGURE_STORAGE */
#endif /* CONFIG_MENDER_CLIENT_ADD_ON_CONFIGURE */

#ifdef CONFIG_MENDER_CLIENT_ADD_ON_TROUBLESHOOT

/**
 * @brief Shell begin callback
 * @param terminal_width Terminal width
 * @param terminal_height Terminal height
 * @return MENDER_OK if the function succeeds, error code otherwise
 */
static mender_err_t
shell_begin_cb(uint16_t terminal_width, uint16_t terminal_height) {

    /* Just print terminal size */
    mender_log_info("Shell connected with width=%d and height=%d", terminal_width, terminal_height);

    return MENDER_OK;
}

/**
 * @brief Shell resize callback
 * @param terminal_width Terminal width
 * @param terminal_height Terminal height
 * @return MENDER_OK if the function succeeds, error code otherwise
 */
static mender_err_t
shell_resize_cb(uint16_t terminal_width, uint16_t terminal_height) {

    /* Just print terminal size */
    mender_log_info("Shell resized with width=%d and height=%d", terminal_width, terminal_height);

    return MENDER_OK;
}

/**
 * @brief Function used to replace a string in the input buffer
 * @param input Input buffer
 * @param search String to be replaced or regex expression
 * @param replace Replacement string
 * @return New string with replacements if the function succeeds, NULL otherwise
 */
static char *
str_replace(char *input, char *search, char *replace) {

    assert(NULL != input);
    assert(NULL != search);
    assert(NULL != replace);

    regex_t    regex;
    regmatch_t match;
    char      *str                   = input;
    char      *output                = NULL;
    size_t     index                 = 0;
    int        previous_match_finish = 0;

    /* Compile expression */
    if (0 != regcomp(&regex, search, REG_EXTENDED)) {
        /* Unable to compile expression */
        mender_log_error("Unable to compile expression '%s'", search);
        return NULL;
    }

    /* Loop until all search string are replaced */
    bool loop = true;
    while (true == loop) {

        /* Search wanted string */
        if (0 != regexec(&regex, str, 1, &match, 0)) {
            /* No more string to be replaced */
            loop = false;
        } else {
            if (match.rm_so != -1) {

                /* Beginning and ending offset of the match */
                int current_match_start  = (int)(match.rm_so + (str - input));
                int current_match_finish = (int)(match.rm_eo + (str - input));

                /* Reallocate output memory */
                char *tmp = (char *)realloc(output, index + (current_match_start - previous_match_finish) + 1);
                if (NULL == tmp) {
                    mender_log_error("Unable to allocate memory");
                    regfree(&regex);
                    free(output);
                    return NULL;
                }
                output = tmp;

                /* Copy string from previous match to the beginning of the current match */
                memcpy(&output[index], &input[previous_match_finish], current_match_start - previous_match_finish);
                index += (current_match_start - previous_match_finish);
                output[index] = 0;

                /* Reallocate output memory */
                if (NULL == (tmp = (char *)realloc(output, index + strlen(replace) + 1))) {
                    mender_log_error("Unable to allocate memory");
                    regfree(&regex);
                    free(output);
                    return NULL;
                }
                output = tmp;

                /* Copy replace string to the output */
                strcat(output, replace);
                index += strlen(replace);

                /* Update previous match ending value */
                previous_match_finish = current_match_finish;
            }
            str += match.rm_eo;
        }
    }

    /* Reallocate output memory */
    char *tmp = (char *)realloc(output, index + (strlen(input) - previous_match_finish) + 1);
    if (NULL == tmp) {
        mender_log_error("Unable to allocate memory");
        regfree(&regex);
        free(output);
        return NULL;
    }
    output = tmp;

    /* Copy the end of the string after the latest match */
    memcpy(&output[index], &input[previous_match_finish], strlen(input) - previous_match_finish);
    index += (strlen(input) - previous_match_finish);
    output[index] = 0;

    /* Release regex */
    regfree(&regex);

    return output;
}

/**
 * @brief Shell write data callback
 * @param data Shell data received
 * @param length Length of the data received
 * @return MENDER_OK if the function succeeds, error code otherwise
 */
static mender_err_t
shell_write_cb(uint8_t *data, size_t length) {

    mender_err_t ret = MENDER_OK;
    char        *buffer, *tmp;

    /* Ensure new line is "\r\n" to have a proper display of the data in the shell */
    if (NULL == (buffer = strndup((char *)data, length))) {
        mender_log_error("Unable to allocate memory");
        ret = MENDER_FAIL;
        goto END;
    }
    if (NULL == (tmp = str_replace(buffer, "\r|\n", "\r\n"))) {
        mender_log_error("Unable to allocate memory");
        ret = MENDER_FAIL;
        goto END;
    }
    buffer = tmp;

    /* Send back the data received */
    if (MENDER_OK != (ret = mender_troubleshoot_shell_print((uint8_t *)buffer, strlen(buffer)))) {
        mender_log_error("Unable to print data to the sehll");
        ret = MENDER_FAIL;
        goto END;
    }

END:

    /* Release memory */
    if (NULL != buffer) {
        free(buffer);
    }

    return ret;
}

/**
 * @brief Shell end callback
 * @return MENDER_OK if the function succeeds, error code otherwise
 */
static mender_err_t
shell_end_cb(void) {

    /* Just print disconnected */
    mender_log_info("Shell disconnected");

    return MENDER_OK;
}

#endif /* CONFIG_MENDER_CLIENT_ADD_ON_TROUBLESHOOT */

/**
 * @brief Signal handler
 * @param signo Signal number
 * @param info Signal information
 * @param arg Unused
 */
static void
sig_handler(int signo, siginfo_t *info, void *arg) {

    assert(NULL != info);
    (void)arg;

    /* Signal handling */
    mender_log_info("Signal '%d' received from process '%d'", signo, info->si_pid);
    if ((SIGINT == signo) || (SIGTERM == signo)) {
        pthread_mutex_lock(&mender_client_events_mutex);
        pthread_cond_signal(&mender_client_events_cond);
        pthread_mutex_unlock(&mender_client_events_mutex);
    }
}

/**
 * @brief Print usage
 * @param argv0 Name of the binary (first argument)
 */
void
print_usage(const char *argv0) {
    printf("usage: %s [options]\n", (strrchr(argv0, '/') ? strrchr(argv0, '/') + 1 : argv0));
    printf("\t--help, -h: Print this help\n");
    printf("\t--mac_address, -m: MAC address\n");
    printf("\t--artifact_name, -a: Artifact name\n");
    printf("\t--device_type, -d: Device type\n");
    printf("\t--tenant_token, -t: Tenant token (optional)\n");
    printf("\t--private_key, -p: Key path (optional)\n");
}

/**
 * @brief Main function
 * @param argc Number of arguments
 * @param argv Arguments
 * @return EXIT_SUCCESS if the program succeeds, EXIT_FAILURE otherwise
 */
int
main(int argc, char **argv) {

    int   ret           = EXIT_SUCCESS;
    char *mac_address   = NULL;
    char *artifact_name = NULL;
    char *device_type   = NULL;
    char *tenant_token  = NULL;
    char *private_key   = NULL;

    /* Initialize sig handler */
    struct sigaction action;
    memset(&action, 0, sizeof(struct sigaction));
    action.sa_sigaction = sig_handler;
    action.sa_flags     = SA_SIGINFO;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    /* Parse options */
    int opt;
    while (-1 != (opt = getopt_long(argc, argv, "hm:a:d:t:", mender_client_options, NULL))) {
        switch (opt) {
            case 'h':
                /* Help */
                print_usage(argv[0]);
                goto END;
                break;
            case 'm':
                /* MAC address */
                mac_address = strdup(optarg);
                break;
            case 'a':
                /* Artifact name */
                artifact_name = strdup(optarg);
                break;
            case 'd':
                /* Device type */
                device_type = strdup(optarg);
                break;
            case 't':
                /* Tenant token */
                tenant_token = strdup(optarg);
                break;
            case 'p':
                /* Key path */
                private_key = strdup(optarg);
                break;
            default:
                /* Unknown option */
                ret = EXIT_FAILURE;
                print_usage(argv[0]);
                goto END;
                break;
        }
    }

    /* Verify mandatory options */
    if ((NULL == mac_address) || (NULL == artifact_name) || (NULL == device_type)) {
        ret = EXIT_FAILURE;
        printf("Missing MAC address, Artifact name, or Device type\n");
        print_usage(argv[0]);
        goto END;
    }

    /* Initialize mender-mcu-client events cond and mutex */
    if (0 != pthread_cond_init(&mender_client_events_cond, NULL)) {
        /* Unable to initialize cond */
        ret = EXIT_FAILURE;
        goto END;
    }
    if (0 != pthread_mutex_init(&mender_client_events_mutex, NULL)) {
        /* Unable to initialize mutex */
        pthread_cond_destroy(&mender_client_events_cond);
        ret = EXIT_FAILURE;
        goto END;
    }

    /* Set key path */
    key_path = private_key;

    /* Initialize mender-client */
    mender_identity.value                             = mac_address;
    mender_client_config_t    mender_client_config    = { .artifact_name                = artifact_name,
                                                          .device_type                  = device_type,
                                                          .host                         = NULL,
                                                          .tenant_token                 = tenant_token,
                                                          .authentication_poll_interval = 0,
                                                          .update_poll_interval         = 0,
                                                          .recommissioning              = false };
    mender_client_callbacks_t mender_client_callbacks = { .network_connect        = network_connect_cb,
                                                          .network_release        = network_release_cb,
                                                          .authentication_success = authentication_success_cb,
                                                          .authentication_failure = authentication_failure_cb,
                                                          .deployment_status      = deployment_status_cb,
                                                          .restart                = restart_cb,
                                                          .get_identity           = get_identity_cb,
                                                          .get_user_provided_keys = get_user_provided_keys_cb };
    if (MENDER_OK != mender_client_init(&mender_client_config, &mender_client_callbacks)) {
        mender_log_error("Unable to initialize mender-client");
        ret = EXIT_FAILURE;
        goto END;
    }

    /* Initialize mender add-ons */
#ifdef CONFIG_MENDER_CLIENT_ADD_ON_CONFIGURE
    mender_configure_config_t    mender_configure_config    = { .refresh_interval = 0 };
    mender_configure_callbacks_t mender_configure_callbacks = {
#ifndef CONFIG_MENDER_CLIENT_CONFIGURE_STORAGE
        .config_updated = config_updated_cb,
#endif /* CONFIG_MENDER_CLIENT_CONFIGURE_STORAGE */
    };
    if (MENDER_OK
        != mender_client_register_addon(
            (mender_addon_instance_t *)&mender_configure_addon_instance, (void *)&mender_configure_config, (void *)&mender_configure_callbacks)) {
        mender_log_error("Unable to register mender-configure add-on");
        ret = EXIT_FAILURE;
        goto RELEASE;
    }
#endif /* CONFIG_MENDER_CLIENT_ADD_ON_CONFIGURE */
#ifdef CONFIG_MENDER_CLIENT_ADD_ON_INVENTORY
    mender_inventory_config_t mender_inventory_config = { .refresh_interval = 0 };
    if (MENDER_OK != mender_client_register_addon((mender_addon_instance_t *)&mender_inventory_addon_instance, (void *)&mender_inventory_config, NULL)) {
        mender_log_error("Unable to register mender-inventory add-on");
        ret = EXIT_FAILURE;
        goto RELEASE;
    }
#endif /* CONFIG_MENDER_CLIENT_ADD_ON_INVENTORY */
#ifdef CONFIG_MENDER_CLIENT_ADD_ON_TROUBLESHOOT
    mender_troubleshoot_config_t    mender_troubleshoot_config = { .healthcheck_interval = 0 };
    mender_troubleshoot_callbacks_t mender_troubleshoot_callbacks
        = { .shell_begin = shell_begin_cb, .shell_resize = shell_resize_cb, .shell_write = shell_write_cb, .shell_end = shell_end_cb };
    if (MENDER_OK
        != mender_client_register_addon(
            (mender_addon_instance_t *)&mender_troubleshoot_addon_instance, (void *)&mender_troubleshoot_config, (void *)&mender_troubleshoot_callbacks)) {
        mender_log_error("Unable to register mender-troubleshoot add-on");
        ret = EXIT_FAILURE;
        goto RELEASE;
    }
#endif /* CONFIG_MENDER_CLIENT_ADD_ON_TROUBLESHOOT */

    /* Finally activate mender client */
    if (MENDER_OK != mender_client_activate()) {
        mender_log_error("Unable to activate mender-client");
        ret = EXIT_FAILURE;
        goto RELEASE;
    }

    /* Wait for mender-mcu-client events */
    pthread_mutex_lock(&mender_client_events_mutex);
    pthread_cond_wait(&mender_client_events_cond, &mender_client_events_mutex);
    pthread_mutex_unlock(&mender_client_events_mutex);

RELEASE:

    /* Deactivate and release mender-client */
    mender_client_deactivate();
    mender_client_exit();

END:

    /* Release memory */
    if (NULL != mac_address) {
        free(mac_address);
    }
    if (NULL != artifact_name) {
        free(artifact_name);
    }
    if (NULL != device_type) {
        free(device_type);
    }
    if (NULL != tenant_token) {
        free(tenant_token);
    }
    free(private_key);

    return ret;
}
