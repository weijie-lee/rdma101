/**
 * RDMA Error Code Diagnostic Tool
 *
 * Features:
 *   - Query all IBV_WC_* error codes: meaning, common causes, fix suggestions
 *   - Query all async event codes (IBV_EVENT_*) meanings
 *   - Supports two usage modes:
 *     a) ./error_diagnosis          -> Print all error codes
 *     b) ./error_diagnosis <code>   -> Query specific error code
 *
 * Build:
 *   gcc -o error_diagnosis error_diagnosis.c -libverbs \
 *       -L../../common -lrdma_utils -I../../common -O2
 *
 * Run:
 *   ./error_diagnosis            # List all error codes
 *   ./error_diagnosis 5          # Query details for status=5
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

/* ========== Color Definitions (terminal) ========== */
#define C_RED     "\033[0;31m"
#define C_GREEN   "\033[0;32m"
#define C_YELLOW  "\033[1;33m"
#define C_BLUE    "\033[0;34m"
#define C_CYAN    "\033[0;36m"
#define C_BOLD    "\033[1m"
#define C_NC      "\033[0m"

/* ========== WC Error Code Info Structure ========== */
struct wc_error_info {
    int         status;         /* IBV_WC_* enum value */
    const char *name;           /* Enum name */
    const char *meaning;        /* Meaning */
    const char *cause1;         /* Common cause 1 */
    const char *cause2;         /* Common cause 2 */
    const char *cause3;         /* Common cause 3 */
    const char *fix;            /* Fix suggestion */
};

/* ========== Complete WC Error Code Table ========== */
static const struct wc_error_info wc_errors[] = {
    {
        .status     = IBV_WC_SUCCESS,                /* 0 */
        .name       = "IBV_WC_SUCCESS",
        .meaning    = "Operation completed successfully",
        .cause1     = "(no error)",
        .cause2     = NULL,
        .cause3     = NULL,
        .fix        = "No fix needed",
    },
    {
        .status     = IBV_WC_LOC_LEN_ERR,            /* 1 */
        .name       = "IBV_WC_LOC_LEN_ERR",
        .meaning    = "Local length error",
        .cause1     = "Receive buffer too small to hold the incoming message",
        .cause2     = "Send SGE total length exceeds QP's max_inline_data (inline mode)",
        .cause3     = "RDMA Read response data exceeds local buffer size",
        .fix        = "Increase recv buffer size; check SGE length matches buffer",
    },
    {
        .status     = IBV_WC_LOC_QP_OP_ERR,          /* 2 */
        .name       = "IBV_WC_LOC_QP_OP_ERR",
        .meaning    = "Local QP operation error",
        .cause1     = "QP configuration incompatible with requested operation (e.g., UD QP doing RDMA Write)",
        .cause2     = "Internal QP consistency error",
        .cause3     = "Exceeded QP's max_rd_atomic / max_dest_rd_atomic limit",
        .fix        = "Check QP type supports the operation; check QP attribute configuration",
    },
    {
        .status     = IBV_WC_LOC_EEC_OP_ERR,         /* 3 */
        .name       = "IBV_WC_LOC_EEC_OP_ERR",
        .meaning    = "Local EEC operation error (RD mode, deprecated)",
        .cause1     = "EE context operation failed (Reliable Datagram mode)",
        .cause2     = NULL,
        .cause3     = NULL,
        .fix        = "RD mode is rarely used, recommend using RC or UD mode",
    },
    {
        .status     = IBV_WC_LOC_PROT_ERR,           /* 4 */
        .name       = "IBV_WC_LOC_PROT_ERR",
        .meaning    = "Local protection domain error",
        .cause1     = "lkey's MR and QP are not in the same PD (protection domain mismatch)",
        .cause2     = "lkey is invalid, deregistered, or expired",
        .cause3     = "Address in SGE exceeds MR registered range",
        .fix        = "Verify QP and MR belong to the same PD; check lkey and address range",
    },
    {
        .status     = IBV_WC_WR_FLUSH_ERR,           /* 5 */
        .name       = "IBV_WC_WR_FLUSH_ERR",
        .meaning    = "WR flushed (QP is in ERROR state)",
        .cause1     = "QP entered ERROR state due to a previous error",
        .cause2     = "This WR and all subsequent WRs will be marked as FLUSH_ERR",
        .cause3     = "This is a secondary error, root cause is the earlier error",
        .fix        = "Find the first non-FLUSH_ERR error and fix it; recover QP: ERROR->RESET->INIT->RTR->RTS",
    },
    {
        .status     = IBV_WC_MW_BIND_ERR,            /* 6 */
        .name       = "IBV_WC_MW_BIND_ERR",
        .meaning    = "Memory Window bind error",
        .cause1     = "MW bind parameters invalid",
        .cause2     = "MW's PD doesn't match QP's PD",
        .cause3     = "Underlying MR has insufficient permissions",
        .fix        = "Check MW parameters and permission configuration",
    },
    {
        .status     = IBV_WC_BAD_RESP_ERR,           /* 7 */
        .name       = "IBV_WC_BAD_RESP_ERR",
        .meaning    = "Bad response (protocol error)",
        .cause1     = "Received unexpected opcode response",
        .cause2     = "Remote sent an illegal response packet",
        .cause3     = "Data corruption in the network",
        .fix        = "Check both QP configurations are consistent; check network connectivity",
    },
    {
        .status     = IBV_WC_LOC_ACCESS_ERR,         /* 8 */
        .name       = "IBV_WC_LOC_ACCESS_ERR",
        .meaning    = "Local access permission error",
        .cause1     = "MR lacks LOCAL_WRITE permission, but recv needs to write",
        .cause2     = "Atomic operation's local buffer lacks write permission",
        .cause3     = NULL,
        .fix        = "Add IBV_ACCESS_LOCAL_WRITE when registering MR",
    },
    {
        .status     = IBV_WC_REM_INV_REQ_ERR,       /* 9 */
        .name       = "IBV_WC_REM_INV_REQ_ERR",
        .meaning    = "Remote invalid request error",
        .cause1     = "Requested remote virtual address is invalid",
        .cause2     = "RDMA operation length is 0 or out of range",
        .cause3     = "Operation violates remote QP configuration limits",
        .fix        = "Check remote address and length; confirm remote QP is properly configured",
    },
    {
        .status     = IBV_WC_REM_ACCESS_ERR,         /* 10 */
        .name       = "IBV_WC_REM_ACCESS_ERR",
        .meaning    = "Remote access permission error",
        .cause1     = "rkey is wrong or has expired",
        .cause2     = "Target address exceeds remote MR registered range",
        .cause3     = "Remote MR lacks REMOTE_WRITE/REMOTE_READ permission",
        .fix        = "Verify rkey is correct; check remote MR's IBV_ACCESS_REMOTE_* flags",
    },
    {
        .status     = IBV_WC_REM_OP_ERR,             /* 11 */
        .name       = "IBV_WC_REM_OP_ERR",
        .meaning    = "Remote operation error",
        .cause1     = "Remote cannot complete the requested operation",
        .cause2     = "Remote QP encountered internal error",
        .cause3     = "Remote has insufficient resources",
        .fix        = "Check remote QP state; check remote logs",
    },
    {
        .status     = IBV_WC_RETRY_EXC_ERR,          /* 12 */
        .name       = "IBV_WC_RETRY_EXC_ERR",
        .meaning    = "Retry count exceeded (peer unreachable)",
        .cause1     = "Peer machine is down or network is disconnected",
        .cause2     = "Peer QP not properly configured (not in RTS state)",
        .cause3     = "Firewall blocking RDMA traffic (RoCE needs UDP 4791 allowed)",
        .fix        = "Confirm peer is online; check network connectivity (ping); check QP state",
    },
    {
        .status     = IBV_WC_RNR_RETRY_EXC_ERR,      /* 13 */
        .name       = "IBV_WC_RNR_RETRY_EXC_ERR",
        .meaning    = "RNR (Receiver Not Ready) retry exceeded",
        .cause1     = "Peer didn't post recv before message arrived",
        .cause2     = "Peer's recv buffers have been completely consumed",
        .cause3     = "rnr_retry set to 0 (no retry)",
        .fix        = "Ensure peer does post_recv first; set rnr_retry=7 (infinite retry)",
    },
    {
        .status     = IBV_WC_LOC_RDD_VIOL_ERR,       /* 14 */
        .name       = "IBV_WC_LOC_RDD_VIOL_ERR",
        .meaning    = "Local RDD violation error (RD mode, deprecated)",
        .cause1     = "Reliable Datagram domain mismatch",
        .cause2     = NULL,
        .cause3     = NULL,
        .fix        = "RD mode is deprecated, recommend using RC mode",
    },
    {
        .status     = IBV_WC_REM_INV_RD_REQ_ERR,     /* 15 */
        .name       = "IBV_WC_REM_INV_RD_REQ_ERR",
        .meaning    = "Remote invalid RD request error (RD mode, deprecated)",
        .cause1     = "RD request is invalid",
        .cause2     = NULL,
        .cause3     = NULL,
        .fix        = "RD mode is deprecated",
    },
    {
        .status     = IBV_WC_REM_ABORT_ERR,           /* 16 */
        .name       = "IBV_WC_REM_ABORT_ERR",
        .meaning    = "Remote abort error",
        .cause1     = "Remote actively aborted the operation",
        .cause2     = "Remote QP was destroyed or reset",
        .cause3     = NULL,
        .fix        = "Check if remote application exited normally",
    },
    {
        .status     = IBV_WC_INV_EECN_ERR,           /* 17 */
        .name       = "IBV_WC_INV_EECN_ERR",
        .meaning    = "Invalid EEC number error (deprecated)",
        .cause1     = "EE context number is invalid",
        .cause2     = NULL,
        .cause3     = NULL,
        .fix        = "Deprecated error code",
    },
    {
        .status     = IBV_WC_INV_EEC_STATE_ERR,      /* 18 */
        .name       = "IBV_WC_INV_EEC_STATE_ERR",
        .meaning    = "Invalid EEC state error (deprecated)",
        .cause1     = "EE context is in error state",
        .cause2     = NULL,
        .cause3     = NULL,
        .fix        = "Deprecated error code",
    },
    {
        .status     = IBV_WC_FATAL_ERR,              /* 19 */
        .name       = "IBV_WC_FATAL_ERR",
        .meaning    = "Fatal error",
        .cause1     = "Unrecoverable transport error",
        .cause2     = "Hardware failure",
        .cause3     = "Serious internal driver error",
        .fix        = "Check dmesg logs; may need to restart driver or system",
    },
    {
        .status     = IBV_WC_RESP_TIMEOUT_ERR,       /* 20 */
        .name       = "IBV_WC_RESP_TIMEOUT_ERR",
        .meaning    = "Response timeout error",
        .cause1     = "Remote did not respond within timeout period",
        .cause2     = "Network congestion causing excessive delay",
        .cause3     = "QP's timeout parameter set too small",
        .fix        = "Increase timeout value; check network latency; check peer status",
    },
    {
        .status     = IBV_WC_GENERAL_ERR,            /* 21 */
        .name       = "IBV_WC_GENERAL_ERR",
        .meaning    = "General error",
        .cause1     = "Other uncategorized error",
        .cause2     = "Transport layer detected unknown issue",
        .cause3     = NULL,
        .fix        = "Check vendor_err field for more info; check dmesg",
    },
    {
        .status     = IBV_WC_TM_ERR,                 /* 22 */
        .name       = "IBV_WC_TM_ERR",
        .meaning    = "Tag Matching error",
        .cause1     = "Tag Matching operation failed",
        .cause2     = "TM parameters invalid",
        .cause3     = NULL,
        .fix        = "Check Tag Matching configuration",
    },
    {
        .status     = IBV_WC_TM_RNDV_INCOMPLETE,     /* 23 */
        .name       = "IBV_WC_TM_RNDV_INCOMPLETE",
        .meaning    = "Tag Matching Rendezvous incomplete",
        .cause1     = "TM Rendezvous protocol interrupted",
        .cause2     = NULL,
        .cause3     = NULL,
        .fix        = "Check TM Rendezvous flow",
    },
};

static const int NUM_WC_ERRORS = sizeof(wc_errors) / sizeof(wc_errors[0]);

/* ========== Async Event Info ========== */
struct async_event_info {
    int         type;
    const char *name;
    const char *meaning;
    const char *level;      /* "Device" / "Port" / "QP" */
};

static const struct async_event_info async_events[] = {
    { IBV_EVENT_CQ_ERR,             "IBV_EVENT_CQ_ERR",
      "CQ overflow error (CQ full but new CQE arrived)", "CQ" },
    { IBV_EVENT_QP_FATAL,           "IBV_EVENT_QP_FATAL",
      "QP fatal error (QP entered ERROR state)", "QP" },
    { IBV_EVENT_QP_REQ_ERR,         "IBV_EVENT_QP_REQ_ERR",
      "QP request error (sender detected error)", "QP" },
    { IBV_EVENT_QP_ACCESS_ERR,      "IBV_EVENT_QP_ACCESS_ERR",
      "QP access error (receiver detected access violation)", "QP" },
    { IBV_EVENT_COMM_EST,           "IBV_EVENT_COMM_EST",
      "Communication established (QP received first request)", "QP" },
    { IBV_EVENT_SQ_DRAINED,         "IBV_EVENT_SQ_DRAINED",
      "SQ drained (all WRs completed in SQD state)", "QP" },
    { IBV_EVENT_PATH_MIG,           "IBV_EVENT_PATH_MIG",
      "Path migration complete (alternate path activated)", "QP" },
    { IBV_EVENT_PATH_MIG_ERR,       "IBV_EVENT_PATH_MIG_ERR",
      "Path migration failed", "QP" },
    { IBV_EVENT_DEVICE_FATAL,       "IBV_EVENT_DEVICE_FATAL",
      "Device fatal error (device reset needed)", "Device" },
    { IBV_EVENT_PORT_ACTIVE,        "IBV_EVENT_PORT_ACTIVE",
      "Port activated (link became ACTIVE)", "Port" },
    { IBV_EVENT_PORT_ERR,           "IBV_EVENT_PORT_ERR",
      "Port error (link became DOWN)", "Port" },
    { IBV_EVENT_LID_CHANGE,         "IBV_EVENT_LID_CHANGE",
      "LID changed (SM reassigned LID)", "Port" },
    { IBV_EVENT_PKEY_CHANGE,        "IBV_EVENT_PKEY_CHANGE",
      "P_Key table changed", "Port" },
    { IBV_EVENT_SM_CHANGE,          "IBV_EVENT_SM_CHANGE",
      "SM (Subnet Manager) changed", "Port" },
    { IBV_EVENT_SRQ_ERR,            "IBV_EVENT_SRQ_ERR",
      "SRQ error", "SRQ" },
    { IBV_EVENT_SRQ_LIMIT_REACHED,  "IBV_EVENT_SRQ_LIMIT_REACHED",
      "SRQ watermark reached (recv WR count below threshold)", "SRQ" },
    { IBV_EVENT_QP_LAST_WQE_REACHED,"IBV_EVENT_QP_LAST_WQE_REACHED",
      "QP last WQE reached (SRQ-associated QP)", "QP" },
    { IBV_EVENT_CLIENT_REREGISTER,  "IBV_EVENT_CLIENT_REREGISTER",
      "Client needs to re-register (SM request)", "Port" },
    { IBV_EVENT_GID_CHANGE,         "IBV_EVENT_GID_CHANGE",
      "GID table changed (network configuration change)", "Port" },
};

static const int NUM_ASYNC_EVENTS = sizeof(async_events) / sizeof(async_events[0]);

/* ========== Print Single WC Error Detail ========== */
static void print_wc_error_detail(const struct wc_error_info *info)
{
    printf("\n");
    printf("  " C_BOLD "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" C_NC "\n");
    printf("  " C_BOLD "Status code: %d" C_NC "\n", info->status);
    printf("  " C_CYAN "Name:   %s" C_NC "\n", info->name);
    printf("  " C_YELLOW "Meaning:   %s" C_NC "\n", info->meaning);

    if (info->cause1 || info->cause2 || info->cause3) {
        printf("  " C_RED "Common causes:" C_NC "\n");
        if (info->cause1) printf("    1. %s\n", info->cause1);
        if (info->cause2) printf("    2. %s\n", info->cause2);
        if (info->cause3) printf("    3. %s\n", info->cause3);
    }

    printf("  " C_GREEN "Fix suggestion:" C_NC "\n");
    printf("    %s\n", info->fix);
}

/* ========== Print All WC Error Codes ========== */
static void print_all_wc_errors(void)
{
    int i;

    printf(C_BOLD C_CYAN "\n╔═══════════════════════════════════════════════════╗\n");
    printf("║     RDMA Work Completion Error Code Reference     ║\n");
    printf("╚═══════════════════════════════════════════════════╝\n" C_NC);

    /* Summary table */
    printf("\n  " C_BOLD "%-6s %-32s %s" C_NC "\n", "Code", "Name", "Meaning");
    printf("  %-6s %-32s %s\n",
           "------", "--------------------------------", "----------");

    for (i = 0; i < NUM_WC_ERRORS; i++) {
        const char *color = (wc_errors[i].status == 0) ? C_GREEN : C_RED;
        printf("  %s%-6d %-32s %s" C_NC "\n",
               color, wc_errors[i].status,
               wc_errors[i].name, wc_errors[i].meaning);
    }

    /* Detailed descriptions */
    printf(C_BOLD "\n━━━ Detailed Descriptions ━━━" C_NC "\n");
    for (i = 0; i < NUM_WC_ERRORS; i++) {
        print_wc_error_detail(&wc_errors[i]);
    }
}

/* ========== Print All Async Events ========== */
static void print_all_async_events(void)
{
    int i;

    printf(C_BOLD C_CYAN "\n╔═══════════════════════════════════════════════════╗\n");
    printf("║       RDMA Async Event Code Reference             ║\n");
    printf("╚═══════════════════════════════════════════════════╝\n" C_NC);

    printf("\n  " C_BOLD "%-6s %-8s %-38s %s" C_NC "\n",
           "Code", "Level", "Name", "Meaning");
    printf("  %-6s %-8s %-38s %s\n",
           "------", "--------", "--------------------------------------", "----------");

    for (i = 0; i < NUM_ASYNC_EVENTS; i++) {
        printf("  %-6d %-8s %-38s %s\n",
               async_events[i].type,
               async_events[i].level,
               async_events[i].name,
               async_events[i].meaning);
    }

    printf("\n  " C_BOLD "Async event handling approach:" C_NC "\n");
    printf("    1. Create a dedicated event handling thread\n");
    printf("    2. Call ibv_get_async_event() to block and wait for events\n");
    printf("    3. Take recovery action based on event type\n");
    printf("    4. Call ibv_ack_async_event() to acknowledge the event\n");
    printf("\n  " C_BOLD "Example code:" C_NC "\n");
    printf("    struct ibv_async_event event;\n");
    printf("    ibv_get_async_event(ctx, &event);\n");
    printf("    printf(\"Event: %%d\\n\", event.event_type);\n");
    printf("    ibv_ack_async_event(&event);\n");
}

/* ========== Query Specific Error Code ========== */
static void query_error_code(int code)
{
    int i;

    /* Search WC error codes first */
    for (i = 0; i < NUM_WC_ERRORS; i++) {
        if (wc_errors[i].status == code) {
            printf(C_BOLD "\nQuery result: WC status code %d\n" C_NC, code);
            print_wc_error_detail(&wc_errors[i]);

            /* Extra: verify with ibv_wc_status_str */
            printf("\n  ibv_wc_status_str(%d) = \"%s\"\n",
                   code, ibv_wc_status_str(code));
            return;
        }
    }

    /* Then search async events */
    for (i = 0; i < NUM_ASYNC_EVENTS; i++) {
        if (async_events[i].type == code) {
            printf(C_BOLD "\nQuery result: async event code %d\n" C_NC, code);
            printf("  Name: %s\n", async_events[i].name);
            printf("  Level: %s\n", async_events[i].level);
            printf("  Meaning: %s\n", async_events[i].meaning);
            return;
        }
    }

    printf(C_RED "\nNo information found for error code %d\n" C_NC, code);
    printf("  WC error code range: 0-%d\n", NUM_WC_ERRORS - 1);
    printf("  Tip: use ./error_diagnosis to see all supported error codes\n");
}

/* ========== Main Function ========== */
int main(int argc, char *argv[])
{
    printf("=== RDMA Error Code Diagnostic Tool ===\n");

    if (argc == 1) {
        /* No arguments: print all error codes */
        print_all_wc_errors();
        print_all_async_events();

        printf(C_BOLD C_CYAN "\n━━━ Usage Tips ━━━\n" C_NC);
        printf("  Query specific error code: ./error_diagnosis <status_code>\n");
        printf("  Example: ./error_diagnosis 5    -> Query IBV_WC_WR_FLUSH_ERR\n");
        printf("  Example: ./error_diagnosis 13   -> Query IBV_WC_RNR_RETRY_EXC_ERR\n");
    } else {
        /* With argument: query specific error code */
        int code = atoi(argv[1]);
        query_error_code(code);
    }

    printf("\n");
    return 0;
}
