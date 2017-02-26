#include <sys/socket.h> // AF_INET, AF_INET6
#include <netinet/in.h> // IPPROTO_UDP, IPPROTO_TCP
#include <string.h> // memcpy
#include <rawrtc.h>
#include "utils.h"
#include "ice_gatherer.h"
#include "ice_candidate.h"
#include "message_buffer.h"
#include "candidate_helper.h"

#define DEBUG_MODULE "ice-gatherer"
#define RAWRTC_DEBUG_MODULE_LEVEL 7 // Note: Uncomment this to debug this module only
#define RAWRTC_DEBUG_ICE_GATHERER 1 // TODO: Remove
#include "debug.h"

static void rawrtc_ice_gather_options_destroy(
        void* const arg
) {
    struct rawrtc_ice_gather_options* const options = arg;

    // Dereference
    list_flush(&options->ice_servers);
}

/*
 * Create a new ICE gather options.
 */
enum rawrtc_code rawrtc_ice_gather_options_create(
        struct rawrtc_ice_gather_options** const optionsp, // de-referenced
        enum rawrtc_ice_gather_policy const gather_policy
) {
    struct rawrtc_ice_gather_options* options;

    // Check arguments
    if (!optionsp) {
        return RAWRTC_CODE_INVALID_ARGUMENT;
    }

    // Allocate
    options = mem_zalloc(sizeof(*options), rawrtc_ice_gather_options_destroy);
    if (!options) {
        return RAWRTC_CODE_NO_MEMORY;
    }

    // Set fields/reference
    options->gather_policy = gather_policy;
    list_init(&options->ice_servers);

    // Set pointer and return
    *optionsp = options;
    return RAWRTC_CODE_SUCCESS;
}

/*
 * Destructor for URLs of the ICE gatherer.
 */
static void rawrtc_ice_server_url_destroy(
        void* const arg
) {
    struct rawrtc_ice_server_url* const url = arg;

    // Dereference
    mem_deref(url->url);
}

/*
 * Copy a URL for the ICE gatherer.
 */
static enum rawrtc_code rawrtc_ice_server_url_create(
        struct rawrtc_ice_server_url** const urlp, // de-referenced
        char* const url_s // copied
) {
    struct rawrtc_ice_server_url* url;
    enum rawrtc_code error;
    char* copy;

    // Check arguments
    if (!urlp || !url_s) {
        return RAWRTC_CODE_INVALID_ARGUMENT;
    }

    // Allocate
    url = mem_zalloc(sizeof(*url), rawrtc_ice_server_url_destroy);
    if (!url) {
        return RAWRTC_CODE_NO_MEMORY;
    }

    // Copy URL
    error = rawrtc_strdup(&copy, url_s);
    if (error) {
        goto out;
    }

    // Set URL
    url->url = copy;

out:
    if (error) {
        mem_deref(url);
    } else {
        // Set pointer
        *urlp = url;
    }
    return error;
}

/*
 * Destructor for an existing ICE server.
 */
static void rawrtc_ice_server_destroy(
        void* const arg
) {
    struct rawrtc_ice_server* const server = arg;

    // Dereference
    list_flush(&server->urls);
    mem_deref(server->username);
    mem_deref(server->credential);
}

/*
 * Add an ICE server to the gather options.
 */
enum rawrtc_code rawrtc_ice_gather_options_add_server(
        struct rawrtc_ice_gather_options* const options,
        char* const * const urls, // copied
        size_t const n_urls,
        char* const username, // nullable, copied
        char* const credential, // nullable, copied
        enum rawrtc_ice_credential_type const credential_type
) {
    struct rawrtc_ice_server* server;
    enum rawrtc_code error = RAWRTC_CODE_SUCCESS;
    size_t i;

    // Check arguments
    if (!options || !urls) {
        return RAWRTC_CODE_INVALID_ARGUMENT;
    }

    // Allocate
    server = mem_zalloc(sizeof(*server), rawrtc_ice_server_destroy);
    if (!server) {
        return RAWRTC_CODE_NO_MEMORY;
    }

    // Copy URLs to list
    list_init(&server->urls);
    for (i = 0; i < n_urls; ++i) {
        struct rawrtc_ice_server_url* url;

        // Ensure URLs aren't null
        if (!urls[i]) {
            error = RAWRTC_CODE_INVALID_ARGUMENT;
            goto out;
        }

        // Copy URL
        error = rawrtc_ice_server_url_create(&url, urls[i]);
        if (error) {
            goto out;
        }

        // Append URL to list
        list_append(&server->urls, &url->le, url);
    }

    // Set fields
    if (credential_type != RAWRTC_ICE_CREDENTIAL_NONE) {
        if (username) {
            error = rawrtc_strdup(&server->username, username);
            if (error) {
                goto out;
            }
        }
        if (credential) {
            error = rawrtc_strdup(&server->credential, credential);
            if (error) {
                goto out;
            }
        }
    }
    server->credential_type = credential_type; // TODO: Validation needed in case TOKEN is used?

    // Add to options
    list_append(&options->ice_servers, &server->le, server);

out:
    if (error) {
        mem_deref(server);
    }
    return error;
}

/*
 * Get the corresponding name for an ICE gatherer state.
 */
char const * const rawrtc_ice_gatherer_state_to_name(
        enum rawrtc_ice_gatherer_state const state
) {
    switch (state) {
        case RAWRTC_ICE_GATHERER_NEW:
            return "new";
        case RAWRTC_ICE_GATHERER_GATHERING:
            return "gathering";
        case RAWRTC_ICE_GATHERER_COMPLETE:
            return "complete";
        case RAWRTC_ICE_GATHERER_CLOSED:
            return "closed";
        default:
            return "???";
    }
}

/*
 * Destructor for an existing ICE gatherer.
 */
static void rawrtc_ice_gatherer_destroy(
        void* const arg
) {
    struct rawrtc_ice_gatherer* const gatherer = arg;

    // Close gatherer
    // TODO: Check effects in case transport has been destroyed due to error in create
    rawrtc_ice_gatherer_close(gatherer);

    // Dereference
    mem_deref(gatherer->stun);
    mem_deref(gatherer->ice);
    list_flush(&gatherer->candidate_helpers);
    list_flush(&gatherer->buffered_messages);
    mem_deref(gatherer->options);
}

/*
 * STUN indication handler.
 * TODO: Do we need this?
 */
static void stun_indication_handler(
        struct stun_msg* message,
        void* arg
) {
    (void) arg;

    // TODO: What needs to be done here?
    stun_msg_dump(message);
}

/*
 * Create a new ICE gatherer.
 */
enum rawrtc_code rawrtc_ice_gatherer_create(
        struct rawrtc_ice_gatherer** const gathererp, // de-referenced
        struct rawrtc_ice_gather_options* const options, // referenced
        rawrtc_ice_gatherer_state_change_handler* const state_change_handler, // nullable
        rawrtc_ice_gatherer_error_handler* const error_handler, // nullable
        rawrtc_ice_gatherer_local_candidate_handler* const local_candidate_handler, // nullable
        void* const arg // nullable
) {
    struct rawrtc_ice_gatherer* gatherer;
    enum rawrtc_code error;

    // Check arguments
    if (!gathererp || !options) {
        return RAWRTC_CODE_INVALID_ARGUMENT;
    }

    // Allocate
    gatherer = mem_zalloc(sizeof(*gatherer), rawrtc_ice_gatherer_destroy);
    if (!gatherer) {
        return RAWRTC_CODE_NO_MEMORY;
    }

    // Set fields/reference
    gatherer->state = RAWRTC_ICE_GATHERER_NEW; // TODO: Raise state (delayed)?
    gatherer->options = mem_ref(options);
    gatherer->state_change_handler = state_change_handler;
    gatherer->error_handler = error_handler;
    gatherer->local_candidate_handler = local_candidate_handler;
    gatherer->arg = arg;
    list_init(&gatherer->buffered_messages);
    list_init(&gatherer->candidate_helpers);

    // Generate random username fragment and password for ICE
    rand_str(gatherer->ice_username_fragment, sizeof(gatherer->ice_username_fragment));
    rand_str(gatherer->ice_password, sizeof(gatherer->ice_password));

    // Set ICE configuration and create trice instance
    // TODO: Add parameters to function arguments?
    gatherer->ice_config.debug = RAWRTC_DEBUG_ICE_GATHERER ? true : false;
    gatherer->ice_config.trace = RAWRTC_DEBUG_ICE_GATHERER ? true : false;
    gatherer->ice_config.ansi = true;
    gatherer->ice_config.enable_prflx = true;
    error = rawrtc_error_to_code(trice_alloc(
            &gatherer->ice, &gatherer->ice_config, ROLE_UNKNOWN,
            gatherer->ice_username_fragment, gatherer->ice_password));
    if (error) {
        goto out;
    }

    // Set STUN configuration and create STUN instance
    // TODO: Add parameters to function arguments?
    gatherer->stun_config.rto = STUN_DEFAULT_RTO;
    gatherer->stun_config.rc = STUN_DEFAULT_RC;
    gatherer->stun_config.rm = STUN_DEFAULT_RM;
    gatherer->stun_config.ti = STUN_DEFAULT_TI;
    gatherer->stun_config.tos = 0x00;
    error = rawrtc_error_to_code(stun_alloc(
            &gatherer->stun, &gatherer->stun_config, stun_indication_handler, NULL));
    if (error) {
        goto out;
    }

out:
    if (error) {
        mem_deref(gatherer);
    } else {
        // Set pointer
        *gathererp = gatherer;
    }
    return error;
}

/*
 * Change the state of the ICE gatherer.
 * Will call the corresponding handler.
 * TODO: https://github.com/w3c/ortc/issues/606
 */
static void set_state(
        struct rawrtc_ice_gatherer* const gatherer,
        enum rawrtc_ice_gatherer_state const state
) {
    // Set state
    gatherer->state = state;

    // Call handler (if any)
    if (gatherer->state_change_handler) {
        gatherer->state_change_handler(state, gatherer->arg);
    }
}

/*
 * Close the ICE gatherer.
 */
enum rawrtc_code rawrtc_ice_gatherer_close(
        struct rawrtc_ice_gatherer* const gatherer
) {
    // Check arguments
    if (!gatherer) {
        return RAWRTC_CODE_INVALID_ARGUMENT;
    }

    // Already closed?
    if (gatherer->state == RAWRTC_ICE_GATHERER_CLOSED) {
        return RAWRTC_CODE_SUCCESS;
    }

    // Stop ICE checklist (if running)
    trice_checklist_stop(gatherer->ice);

    // Remove ICE agent
    gatherer->ice = mem_deref(gatherer->ice);

    // Set state to closed and return
    set_state(gatherer, RAWRTC_ICE_GATHERER_CLOSED);
}

/*
 * Handle received UDP messages.
 */
static bool udp_receive_handler(
        struct sa * const source,
        struct mbuf* const buffer,
        void* const arg
) {
    struct rawrtc_ice_gatherer* const gatherer = arg;
    enum rawrtc_code error;

    // Allocate context and copy source address
    void* const context = mem_zalloc(sizeof(*source), NULL);
    if (!context) {
        error = RAWRTC_CODE_NO_MEMORY;
        goto out;
    }
    memcpy(context, source, sizeof(*source));

    // Buffer message
    error = rawrtc_message_buffer_append(&gatherer->buffered_messages, buffer, context);
    if (error) {
        goto out;
    }

    // Done
    DEBUG_PRINTF("Buffered UDP packet of size %zu\n", mbuf_get_left(buffer));

out:
    if (error) {
        DEBUG_WARNING("Could not buffer UDP packet, reason: %s\n", rawrtc_code_to_str(error));
    }

    // Dereference
    mem_deref(context);

    // Handled
    return true;
}

/*
 * Add local candidate, gather server reflexive and relay candidates.
 */
static void add_candidate(
        struct rawrtc_ice_gatherer* const gatherer,
        struct sa const* const address,
        enum rawrtc_ice_protocol const protocol,
        enum ice_tcptype const tcp_type
) {
    uint32_t priority;
    int const ipproto = rawrtc_ice_protocol_to_ipproto(protocol);
    struct ice_lcand* re_candidate;
    int err;
    struct rawrtc_candidate_helper* candidate_helper;
    enum rawrtc_code error;
    struct rawrtc_ice_candidate* candidate;

    // Add host candidate
    priority = rawrtc_ice_candidate_calculate_priority(
            ICE_CAND_TYPE_HOST, ipproto, tcp_type);
    // TODO: Set component id properly
    err = trice_lcand_add(
            &re_candidate, gatherer->ice, 1, ipproto, priority, address,
            NULL, ICE_CAND_TYPE_HOST, NULL, tcp_type, NULL, RAWRTC_LAYER_ICE);
    if (err) {
        DEBUG_WARNING("Could not add candidate, reason: %m\n", err);
        return;
    }
    DEBUG_PRINTF("Added %s candidate for interface %j\n", rawrtc_ice_protocol_to_str(protocol),
                 address);

    // Attach temporary UDP helper
    error = rawrtc_candidate_helper_attach(
            &candidate_helper, gatherer->ice, re_candidate, udp_receive_handler, gatherer);
    if (error) {
        DEBUG_WARNING("Could not attach candidate helper, reason: %s\n",
                      rawrtc_code_to_str(error));
    } else {
        // Add to list
        list_append(&gatherer->candidate_helpers, &candidate_helper->le, candidate_helper);
    }

    // Create ICE candidate, call local candidate handler, unreference ICE candidate
    error = rawrtc_ice_candidate_create_from_local_candidate(&candidate, re_candidate);
    if (error) {
        DEBUG_WARNING("Could not create local candidate instance: %s\n",
                      rawrtc_code_to_str(error));
    } else {
        if (gatherer->local_candidate_handler) {
            gatherer->local_candidate_handler(candidate, NULL, gatherer->arg);
        }
        mem_deref(candidate);
    }

    // Gather server reflexive candidate

    // TODO: Gather srflx candidates
    DEBUG_PRINTF("TODO: Gather srflx candidates for %j\n", address);
    // TODO: Gather relay candidates
    DEBUG_PRINTF("TODO: Gather relay candidates for %j\n", address);
}

/*
 * Local interfaces callback.
 * TODO: Consider ICE gather policy
 * TODO: https://tools.ietf.org/html/draft-ietf-rtcweb-ip-handling-01
  */
static bool interface_handler(
        char const* const interface,
        struct sa const* const address,
        void* const arg
) {
    int af;
    struct rawrtc_ice_gatherer* const gatherer = arg;

    // Unused
    (void) interface;

    // Ignore loopback and linklocal addresses
    if (sa_is_linklocal(address) || sa_is_loopback(address)) {
        return false; // Continue gathering
    }

    // Skip IPv4, IPv6?
    // TODO: Get config from struct
    af = sa_af(address);
    if (!rawrtc_default_config.ipv6_enable && af == AF_INET6
            || !rawrtc_default_config.ipv4_enable && af == AF_INET) {
        return false; // Continue gathering
    }

    // TODO: Ignore interfaces gatherered twice

    DEBUG_PRINTF("Gathered local interface %j\n", address);

    // Add UDP candidate
    if (rawrtc_default_config.udp_enable) {
        add_candidate(gatherer, address, RAWRTC_ICE_PROTOCOL_UDP, ICE_TCP_ACTIVE);
    }

    // Add TCP candidate
    if (rawrtc_default_config.tcp_enable) {
        // TODO
        //add_candidate(gatherer, address, RAWRTC_ICE_PROTOCOL_TCP, ICE_TCP_SO);
    }

    // Continue gathering
    return false;
}

/*
 * Start gathering using an ICE gatherer.
 */
enum rawrtc_code rawrtc_ice_gatherer_gather(
        struct rawrtc_ice_gatherer* const gatherer,
        struct rawrtc_ice_gather_options* options // referenced, nullable
) {
    // Check arguments
    if (!gatherer) {
        return RAWRTC_CODE_INVALID_ARGUMENT;
    }
    if (!options) {
        options = gatherer->options;
    }

    // Check state
    if (gatherer->state == RAWRTC_ICE_GATHERER_CLOSED) {
        return RAWRTC_CODE_INVALID_STATE;
    }

    // Already gathering?
    if (gatherer->state == RAWRTC_ICE_GATHERER_GATHERING) {
        return RAWRTC_CODE_SUCCESS;
    }

    // Update state
    set_state(gatherer, RAWRTC_ICE_GATHERER_GATHERING);

    // Start gathering host candidates
    if (options->gather_policy != RAWRTC_ICE_GATHER_NOHOST) {
        net_if_apply(interface_handler, gatherer);
    }

    // Gathering complete
    // TODO: Complete after gathering srflx and relay candidates
    gatherer->local_candidate_handler(NULL, NULL, gatherer->arg);
    set_state(gatherer, RAWRTC_ICE_GATHERER_COMPLETE);

    // TODO: Debug only
    DEBUG_PRINTF("%H", trice_debug, gatherer->ice);
    return RAWRTC_CODE_SUCCESS;
}

/*
 * Get local ICE parameters of an ICE gatherer.
 */
enum rawrtc_code rawrtc_ice_gatherer_get_local_parameters(
        struct rawrtc_ice_parameters** const parametersp, // de-referenced
        struct rawrtc_ice_gatherer* const gatherer
) {
    // Check arguments
    if (!parametersp || !gatherer) {
        return RAWRTC_CODE_INVALID_ARGUMENT;
    }

    // Check state
    if (gatherer->state == RAWRTC_ICE_GATHERER_CLOSED) {
        return RAWRTC_CODE_INVALID_STATE;
    }

    // Create and return ICE parameters instance
    return rawrtc_ice_parameters_create(
            parametersp, gatherer->ice_username_fragment, gatherer->ice_password, false);
}

/*
 * Destructor for an existing local candidates array.
 */
static void rawrtc_ice_gatherer_local_candidates_destroy(
        void* const arg
) {
    struct rawrtc_ice_candidates* const candidates = arg;
    size_t i;

    // Dereference each item
    for (i = 0; i < candidates->n_candidates; ++i) {
        mem_deref(candidates->candidates[i]);
    }
}

/*
 * Get local ICE candidates of an ICE gatherer.
 */
enum rawrtc_code rawrtc_ice_gatherer_get_local_candidates(
        struct rawrtc_ice_candidates** const candidatesp, // de-referenced
        struct rawrtc_ice_gatherer* const gatherer
) {
    size_t n;
    struct rawrtc_ice_candidates* candidates;
    struct le* le;
    size_t i;
    enum rawrtc_code error = RAWRTC_CODE_SUCCESS;

    // Check arguments
    if (!candidatesp || !gatherer) {
        return RAWRTC_CODE_INVALID_ARGUMENT;
    }

    // Get length
    n = list_count(trice_lcandl(gatherer->ice));

    // Allocate & set length immediately
    candidates = mem_zalloc(sizeof(*candidates) + (sizeof(struct rawrtc_ice_candidate*) * n),
                            rawrtc_ice_gatherer_local_candidates_destroy);
    if (!candidates) {
        return RAWRTC_CODE_NO_MEMORY;
    }
    candidates->n_candidates = n;

    // Copy each ICE candidate
    for (le = list_head(trice_lcandl(gatherer->ice)), i = 0; le != NULL; le = le->next, ++i) {
        struct ice_lcand* re_candidate = le->data;

        // Create ICE candidate
        error = rawrtc_ice_candidate_create_from_local_candidate(
                &candidates->candidates[i], re_candidate);
        if (error) {
            goto out;
        }
    }

out:
    if (error) {
        mem_deref(candidates);
    } else {
        // Set pointers
        *candidatesp = candidates;
    }
    return error;
}
