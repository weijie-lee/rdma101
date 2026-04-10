/**
 * RDMA WC Error Code Lookup Tool (pure printf version, no RDMA headers needed)
 *
 * Function:
 *   - No arguments: print detailed descriptions of all common WC status codes
 *   - With argument: query a specific error code (supports numeric and name)
 *
 * Usage:
 *   ./error_cheatsheet                          # Print all error codes
 *   ./error_cheatsheet 12                       # Query by error code (numeric)
 *   ./error_cheatsheet IBV_WC_RETRY_EXC_ERR     # Query by error name
 *   ./error_cheatsheet RETRY_EXC_ERR            # Abbreviated (omit IBV_WC_ prefix)
 *   ./error_cheatsheet RETRY                    # Fuzzy search
 *
 * Each error includes:
 *   - Error code value and enum name (IBV_WC_*)
 *   - Meaning/description
 *   - Top 3 most common causes
 *   - Fix suggestions
 *
 * Build (no libibverbs needed):
 *   gcc -Wall -O2 -g -o error_cheatsheet error_cheatsheet.c
 */

#include <stdio.h>      /* printf, fprintf */
#include <stdlib.h>     /* strtol */
#include <string.h>     /* strcmp, strcasecmp, strcasestr, strlen */

/* ========== Terminal Color Definitions ========== */
#define CLR_RED     "\033[0;31m"    /* Red (error) */
#define CLR_GREEN   "\033[0;32m"    /* Green (success) */
#define CLR_YELLOW  "\033[1;33m"    /* Yellow (cause) */
#define CLR_BLUE    "\033[0;34m"    /* Blue (fix) */
#define CLR_CYAN    "\033[0;36m"    /* Cyan (title) */
#define CLR_BOLD    "\033[1m"       /* Bold */
#define CLR_RESET   "\033[0m"       /* Reset */

/* ========== Error Entry Structure ========== */
struct error_entry {
    int         code;           /* Status code value */
    const char *name;           /* Enum name (IBV_WC_*) */
    const char *meaning;        /* Meaning */
    const char *cause1;         /* Common cause 1 */
    const char *cause2;         /* Common cause 2 */
    const char *cause3;         /* Common cause 3 */
    const char *fix;            /* Fix suggestion */
};

/* ========== All WC Status Code Definitions (hardcoded, no infiniband/verbs.h needed) ========== */
static const struct error_entry errors[] = {
    /* --- 0: Success --- */
    {
        .code       = 0,
        .name       = "IBV_WC_SUCCESS",
        .meaning    = "Operation completed successfully",
        .cause1     = "Not an error; indicates the WR was successfully processed by the HCA",
        .cause2     = "Send/Recv/RDMA Read/Write/Atomic all completed normally",
        .cause3     = "Data has been reliably sent or received into the target buffer",
        .fix        = "No fix needed. Check wc.byte_len (recv side) and wc.opcode for details."
    },
    /* --- 1: Local length error --- */
    {
        .code       = 1,
        .name       = "IBV_WC_LOC_LEN_ERR",
        .meaning    = "Local length error - data length exceeds limit",
        .cause1     = "Send data exceeds QP's max_inline_data or path MTU limit",
        .cause2     = "Recv buffer (Recv WR's SGE) too small to hold received data",
        .cause3     = "SGE length field set incorrectly (0 or exceeds MR range)",
        .fix        = "Check SGE.length. Recv buffer should be >= send data size. Verify path_mtu."
    },
    /* --- 2: Local QP operation error --- */
    {
        .code       = 2,
        .name       = "IBV_WC_LOC_QP_OP_ERR",
        .meaning    = "Local QP operation error - QP config or state incorrect",
        .cause1     = "QP not transitioned to RTS state before calling ibv_post_send()",
        .cause2     = "QP type doesn't support the requested operation (e.g., UD QP doing RDMA Write)",
        .cause3     = "Send operation parameters invalid (e.g., num_sge=0, invalid opcode)",
        .fix        = "Ensure QP is in RTS state before sending. Use ibv_query_qp() to check QP state."
    },
    /* --- 3: Local EEC operation error (RD QP, very rare) --- */
    {
        .code       = 3,
        .name       = "IBV_WC_LOC_EEC_OP_ERR",
        .meaning    = "Local EEC operation error (RD QP related, very rare)",
        .cause1     = "EEC (End-to-End Context) state incorrect",
        .cause2     = "Only occurs with RD (Reliable Datagram) QP type",
        .cause3     = "RD QP is rarely used; most environments won't encounter this",
        .fix        = "If using RC QP, you should not see this error. Check QP type."
    },
    /* --- 4: Local protection error --- */
    {
        .code       = 4,
        .name       = "IBV_WC_LOC_PROT_ERR",
        .meaning    = "Local protection error - memory access permission mismatch",
        .cause1     = "MR registered without IBV_ACCESS_LOCAL_WRITE but NIC needs local write",
        .cause2     = "SGE lkey doesn't match the actual MR (wrong lkey used)",
        .cause3     = "Memory address accessed is outside the MR's registered range",
        .fix        = "Check ibv_reg_mr() access_flags. Recv operations must have LOCAL_WRITE."
    },
    /* --- 5: WR flushed --- */
    {
        .code       = 5,
        .name       = "IBV_WC_WR_FLUSH_ERR",
        .meaning    = "WR flushed - QP has entered Error state",
        .cause1     = "A previous operation failed, causing QP to transition to Error state",
        .cause2     = "In Error state, all outstanding WRs are flushed with this error",
        .cause3     = "Remote disconnected or QP manually set to Error via ibv_modify_qp",
        .fix        = "FLUSH_ERR is NOT the root cause! Find the first non-FLUSH error before it. Reset QP or rebuild connection."
    },
    /* --- 6: MW bind error --- */
    {
        .code       = 6,
        .name       = "IBV_WC_MW_BIND_ERR",
        .meaning    = "Memory Window bind error",
        .cause1     = "MW bind operation parameters invalid",
        .cause2     = "MR doesn't support MW binding (MW bind flag not specified at registration)",
        .cause3     = "Memory Window feature is rarely used",
        .fix        = "Most programs don't use MW. If using, check MR registration and MW bind parameters."
    },
    /* --- 7: Bad response --- */
    {
        .code       = 7,
        .name       = "IBV_WC_BAD_RESP_ERR",
        .meaning    = "Received unexpected/corrupted response packet",
        .cause1     = "Response packet doesn't match expected operation type",
        .cause2     = "Data corruption on the network (CRC check failure, etc.)",
        .cause3     = "Remote HCA firmware bug or hardware failure",
        .fix        = "Check network link quality (ibstatus, perfquery). May need to replace cable/fiber."
    },
    /* --- 8: Local access error --- */
    {
        .code       = 8,
        .name       = "IBV_WC_LOC_ACCESS_ERR",
        .meaning    = "Local access error - lkey invalid or MR already deregistered",
        .cause1     = "Used lkey from an MR that was already deregistered via ibv_dereg_mr()",
        .cause2     = "lkey value wrong (typo or used lkey from a different MR)",
        .cause3     = "MR registered address range doesn't cover the SGE specified range",
        .fix        = "Ensure SGE.lkey corresponds to the correct, still-registered MR. Check address range."
    },
    /* --- 9: Remote invalid request --- */
    {
        .code       = 9,
        .name       = "IBV_WC_REM_INV_REQ_ERR",
        .meaning    = "Remote invalid request - remote considers request illegal",
        .cause1     = "RDMA Write/Read target address outside remote MR registered range",
        .cause2     = "Requested operation type not supported by remote QP (e.g., Atomic not enabled)",
        .cause3     = "Connection parameters wrong causing request misrouting (dest_qp_num mismatch)",
        .fix        = "Print and compare rdma_endpoint info on both sides. Check remote_addr + length range."
    },
    /* --- 10: Remote access error --- */
    {
        .code       = 10,
        .name       = "IBV_WC_REM_ACCESS_ERR",
        .meaning    = "Remote access error - remote MR permissions insufficient",
        .cause1     = "RDMA Write but remote MR missing IBV_ACCESS_REMOTE_WRITE",
        .cause2     = "RDMA Read but remote MR missing IBV_ACCESS_REMOTE_READ",
        .cause3     = "Atomic but remote MR missing REMOTE_ATOMIC; or rkey mismatch/expired",
        .fix        = "Ensure remote ibv_reg_mr() includes corresponding REMOTE_* permissions. Check rkey exchange."
    },
    /* --- 11: Remote operation error --- */
    {
        .code       = 11,
        .name       = "IBV_WC_REM_OP_ERR",
        .meaning    = "Remote operation error - remote internal processing failed",
        .cause1     = "Remote HCA encountered internal error processing the request",
        .cause2     = "Remote QP has entered Error state",
        .cause3     = "Remote resource exhaustion (e.g., SRQ overflow, memory mapping invalidated)",
        .fix        = "Check remote dmesg and QP state. Remote program may have crashed."
    },
    /* --- 12: Retry exceeded --- */
    {
        .code       = 12,
        .name       = "IBV_WC_RETRY_EXC_ERR",
        .meaning    = "Retry count exceeded - network unreachable or remote down",
        .cause1     = "Physical network connection broken (cable/fiber/switch failure)",
        .cause2     = "Remote program exited, machine powered off, or QP destroyed",
        .cause3     = "Connection parameters wrong: dest_qp_num / LID / GID incorrect",
        .fix        = "1) Ping remote to confirm network 2) ibstatus to check port 3) Print and compare endpoint info"
    },
    /* --- 13: RNR retry exceeded --- */
    {
        .code       = 13,
        .name       = "IBV_WC_RNR_RETRY_EXC_ERR",
        .meaning    = "RNR retry exceeded - remote has no Posted Recv (recv queue empty)",
        .cause1     = "Most common: remote forgot ibv_post_recv() before data was sent",
        .cause2     = "Remote Recv WRs consumed and not replenished in time",
        .cause3     = "rnr_retry set to 0 (no retry); should be set to 7 (infinite retry)",
        .fix        = "Ensure receiver Posts Recv before sender Posts Send. Set rnr_retry=7."
    },
    /* --- 14: Local RDD violation (RD QP, very rare) --- */
    {
        .code       = 14,
        .name       = "IBV_WC_LOC_RDD_VIOL_ERR",
        .meaning    = "Local RDD violation (RD QP related, very rare)",
        .cause1     = "RD QP Reliable Datagram Domain mismatch",
        .cause2     = "Only occurs with RD QP type",
        .cause3     = "Most environments don't support RD QP",
        .fix        = "You won't encounter this if not using RD QP."
    },
    /* --- 15: Remote invalid RD request --- */
    {
        .code       = 15,
        .name       = "IBV_WC_REM_INV_RD_REQ_ERR",
        .meaning    = "Remote invalid RD request (RD QP related, very rare)",
        .cause1     = "Remote RD request parameters invalid",
        .cause2     = "Only occurs with RD QP type",
        .cause3     = "Most environments don't support RD QP",
        .fix        = "You won't encounter this if not using RD QP."
    },
    /* --- 19: Fatal error --- */
    {
        .code       = 19,
        .name       = "IBV_WC_FATAL_ERR",
        .meaning    = "Fatal error - HCA hardware failure",
        .cause1     = "HCA (NIC) internal unrecoverable hardware error",
        .cause2     = "HCA firmware crash or anomaly",
        .cause3     = "PCIe bus error or unstable power supply",
        .fix        = "Check dmesg for hardware error logs. Try restarting driver. May need to replace NIC."
    },
    /* --- 20: Response timeout --- */
    {
        .code       = 20,
        .name       = "IBV_WC_RESP_TIMEOUT_ERR",
        .meaning    = "Response timeout - timed out waiting for remote response",
        .cause1     = "Similar to RETRY_EXC_ERR, remote response timed out",
        .cause2     = "Severe network congestion causing latency to exceed timeout threshold",
        .cause3     = "Timeout parameter in QP RTR->RTS set too small",
        .fix        = "Increase timeout parameter (e.g., from 14 to 18). Check network latency and packet loss."
    },
};

/* Total number of error entries */
#define NUM_ERRORS  (sizeof(errors) / sizeof(errors[0]))

/* ========== Print a Single Error Entry ========== */
static void print_error(const struct error_entry *e)
{
    /* Error code name and value (green for success, red for error) */
    if (e->code == 0) {
        printf(CLR_GREEN CLR_BOLD "  [%2d] %s" CLR_RESET "\n",
               e->code, e->name);
    } else {
        printf(CLR_RED CLR_BOLD "  [%2d] %s" CLR_RESET "\n",
               e->code, e->name);
    }

    /* Meaning */
    printf("       Meaning: %s\n", e->meaning);

    /* Three common causes */
    printf(CLR_YELLOW);
    printf("       Cause 1: %s\n", e->cause1);
    printf("       Cause 2: %s\n", e->cause2);
    printf("       Cause 3: %s\n", e->cause3);
    printf(CLR_RESET);

    /* Fix suggestion */
    printf(CLR_BLUE "       Fix: %s\n" CLR_RESET, e->fix);

    printf("\n");
}

/* ========== Print All Error Codes ========== */
static void print_all_errors(void)
{
    printf("\n");
    printf(CLR_CYAN CLR_BOLD
           "============================================================\n"
           "  RDMA WC (Work Completion) Error Code Reference\n"
           "  %zu common status codes (lookup version, no libibverbs needed)\n"
           "============================================================\n\n"
           CLR_RESET, NUM_ERRORS);

    /* Print the 5 most commonly encountered errors first */
    printf(CLR_CYAN CLR_BOLD "--- Top 5 Most Common Errors ---\n\n" CLR_RESET);

    int common_codes[] = {12, 13, 10, 5, 4};   /* Ranked by actual frequency */
    for (int i = 0; i < 5; i++) {
        for (size_t j = 0; j < NUM_ERRORS; j++) {
            if (errors[j].code == common_codes[i]) {
                print_error(&errors[j]);
                break;
            }
        }
    }

    /* Full list */
    printf(CLR_CYAN CLR_BOLD "--- Full Error Code List ---\n\n" CLR_RESET);

    for (size_t i = 0; i < NUM_ERRORS; i++) {
        print_error(&errors[i]);
    }

    /* Debug tips */
    printf(CLR_CYAN
           "============================================================\n"
           "  Debug Tips:\n"
           "    1. Use ibv_wc_status_str(wc.status) in code for English description\n"
           "    2. Use print_wc_detail(&wc) to print all WC fields\n"
           "    3. WR_FLUSH_ERR (5) is not the root cause; find the first error before it\n"
           "    4. Non-zero wc.vendor_err contains vendor-specific error details\n"
           "    5. RETRY_EXC (12) and RNR_RETRY_EXC (13) are the two most common errors\n"
           "============================================================\n"
           CLR_RESET "\n");
}

/* ========== Search for a Specific Error by Number or Name ========== */
static int search_error(const char *query)
{
    /* Try to parse as numeric */
    char *endptr;
    long code = strtol(query, &endptr, 10);
    if (*endptr == '\0') {
        /* Input is numeric */
        for (size_t i = 0; i < NUM_ERRORS; i++) {
            if (errors[i].code == (int)code) {
                printf("\n");
                print_error(&errors[i]);
                return 0;
            }
        }
        fprintf(stderr, CLR_RED "Error code not found: %ld\n" CLR_RESET, code);
        fprintf(stderr, "Valid error code values: 0~15, 19, 20\n");
        return 1;
    }

    /* Input is a name -- supports full name and abbreviated form without IBV_WC_ prefix */
    for (size_t i = 0; i < NUM_ERRORS; i++) {
        /* Full name match (case-insensitive) */
        if (strcasecmp(query, errors[i].name) == 0) {
            printf("\n");
            print_error(&errors[i]);
            return 0;
        }

        /* Abbreviated match: omit "IBV_WC_" prefix (7 characters) */
        if (strlen(errors[i].name) > 7) {
            const char *short_name = errors[i].name + 7;
            if (strcasecmp(query, short_name) == 0) {
                printf("\n");
                print_error(&errors[i]);
                return 0;
            }
        }
    }

    /* Fuzzy match: substring search */
    int found = 0;
    for (size_t i = 0; i < NUM_ERRORS; i++) {
        if (strcasestr(errors[i].name, query) != NULL ||
            strstr(errors[i].meaning, query) != NULL) {
            if (!found) {
                printf("\n" CLR_CYAN "Fuzzy match results:\n\n" CLR_RESET);
            }
            print_error(&errors[i]);
            found = 1;
        }
    }

    if (found) return 0;

    /* Not found */
    fprintf(stderr, CLR_RED "No match found: %s\n" CLR_RESET, query);
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  ./error_cheatsheet                      # Print all\n");
    fprintf(stderr, "  ./error_cheatsheet 12                   # Numeric query\n");
    fprintf(stderr, "  ./error_cheatsheet IBV_WC_RETRY_EXC_ERR # Full name\n");
    fprintf(stderr, "  ./error_cheatsheet RETRY                # Fuzzy search\n");
    return 1;
}

/* ========== Main Function ========== */
int main(int argc, char *argv[])
{
    if (argc == 1) {
        /* No arguments: print all error codes */
        print_all_errors();
        return 0;
    }

    if (argc == 2) {
        /* One argument: query specific error code */
        return search_error(argv[1]);
    }

    /* Too many arguments */
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s                          # Print all error codes\n", argv[0]);
    fprintf(stderr, "  %s <error name or number>    # Query specific error\n", argv[0]);
    fprintf(stderr, "\n");
    fprintf(stderr, "Examples:\n");
    fprintf(stderr, "  %s 12                       # Query by number\n", argv[0]);
    fprintf(stderr, "  %s IBV_WC_REM_ACCESS_ERR    # Full name\n", argv[0]);
    fprintf(stderr, "  %s REM_ACCESS_ERR           # Omit IBV_WC_ prefix\n", argv[0]);
    fprintf(stderr, "  %s RETRY                    # Fuzzy search\n", argv[0]);
    return 1;
}
