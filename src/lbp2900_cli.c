/*
 * lbp2900_cli.c — Lightweight Command-Line Controller & Drop-In lp/lpr/lpstat Replacement
 *
 * Works both as:
 *   1. lbp2900ctl <command> [args...]
 *   2. Drop-in replacement for lp, lpr, lpstat, lpq, lprm when invoked by symlink/basename
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <cups/cups.h>
#include "capt_defs.h"

static int send_ctrl_cmd(const char *cmd, char *reply, size_t reply_sz) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, LBP2900_CTRL_SOCK, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    char buf[1024];
    int len = snprintf(buf, sizeof(buf), "%s\n", cmd);
    if (write(fd, buf, len) < 0) {
        close(fd);
        return -1;
    }
    if (reply && reply_sz > 0) {
        size_t off = 0;
        while (off < reply_sz - 1) {
            ssize_t n = read(fd, reply + off, reply_sz - 1 - off);
            if (n <= 0) break;
            off += (size_t)n;
        }
        reply[off] = '\0';
    }
    close(fd);
    return 0;
}

static void print_usage(const char *prog) {
    printf("Canon LASER SHOT LBP-2900 CAPT Standalone Controller (%s)\n\n", prog);
    printf("Usage:\n");
    printf("  %s status                    Show live printer hardware & daemon status\n", prog);
    printf("  %s queue                     Show pending FIFO print queue\n", prog);
    printf("  %s purge                     Purge all pending queued print jobs\n", prog);
    printf("  %s print <file> [title]      Print PDF/PS/Image/Text file via CAPT engine\n", prog);
    printf("  %s test-page [pages]         Print 1-page or N-page test document\n", prog);
    printf("  %s prefs                     Show current printing preferences\n", prog);
    printf("  %s set <key=val> [...]       Update preferences (density=1..15, toner_save=0|1, copies=1..99)\n", prog);
    printf("  %s pause                     Hold/pause job queue\n", prog);
    printf("  %s resume                    Resume job queue\n", prog);
    printf("  %s resume-paper              Confirm paper loaded & resume printing\n", prog);
    printf("  %s cancel                    Cancel active print job\n", prog);
    printf("  %s clean                     Run fixing assembly cleaning cycle\n", prog);
    printf("  %s sim-disconnect <1|0>      Simulate USB disconnect/reconnect for FIFO testing\n", prog);
    printf("  %s sim-fault <jam|door|none> Simulate Paper Jam / Cover Open / Clear for recovery testing\n", prog);
    printf("  %s ui                        Open Canon CAPT Status Monitor & Preferences UI\n", prog);
}

static int submit_file_via_ctrl(const char *file, const char *title) {
    char abspath[PATH_MAX];
    if (!realpath(file, abspath)) {
        snprintf(abspath, sizeof(abspath), "%s", file);
    }
    char cmd[8192];
    char reply[1024];
    snprintf(cmd, sizeof(cmd), "PRINT %s|%s", abspath, title ? title : file);
    if (send_ctrl_cmd(cmd, reply, sizeof(reply)) < 0) {
        fprintf(stderr, "Error: Cannot connect to lbp2900d at %s\n", LBP2900_CTRL_SOCK);
        return 1;
    }
    if (strncmp(reply, "ERR", 3) == 0) {
        fprintf(stderr, "Print error: %s", reply);
        return 1;
    }
    unsigned jid = 0;
    int usb_conn = 1;
    int pend = 0;
    const char *p_jid = strstr(reply, "job_id=");
    const char *p_usb = strstr(reply, "usb_connected=");
    const char *p_pnd = strstr(reply, "pending_jobs=");
    if (p_jid) jid = (unsigned)atoi(p_jid + 7);
    if (p_usb) usb_conn = atoi(p_usb + 14);
    if (p_pnd) pend = atoi(p_pnd + 13);

    if (!usb_conn) {
        printf("[NO CONNECTION / DISCONNECTED] Canon LBP-2900 is offline!\n");
        printf("Job #%u (%s) held in pending FIFO queue (%d waiting) and will print automatically upon USB reconnection.\n",
               jid, title ? title : file, pend);
    } else {
        printf("Submitted Job #%u (%s) to Canon_LBP2900 (%d in FIFO queue)\n",
               jid, title ? title : file, pend);
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *prog = strrchr(argv[0], '/');
    prog = prog ? prog + 1 : argv[0];

    /* Drop-in lpstat / lpq behavior */
    if (strcmp(prog, "lpstat") == 0 || strcmp(prog, "lpq") == 0) {
        char reply[2048];
        if (send_ctrl_cmd("STATUS", reply, sizeof(reply)) == 0) {
            printf("Canon_LBP2900 accepting requests since daemon start\n");
            printf("device for Canon_LBP2900: usb://Canon/LBP2900\n");
            printf("system default destination: Canon_LBP2900\n");
            printf("status:\n%s", reply);
            if (strcmp(prog, "lpq") == 0) {
                char qrep[2048];
                if (send_ctrl_cmd("QUEUE", qrep, sizeof(qrep)) == 0)
                    printf("queue:\n%s", qrep);
            }
            return 0;
        } else {
            fprintf(stderr, "lbp2900d is not running.\n");
            return 1;
        }
    }

    /* Drop-in lprm / cancel behavior */
    if (strcmp(prog, "lprm") == 0 || strcmp(prog, "cancel") == 0) {
        char reply[256];
        if (send_ctrl_cmd("CANCEL", reply, sizeof(reply)) == 0) {
            printf("Cancelled active job on Canon_LBP2900.\n");
            return 0;
        }
        return 1;
    }

    /* Drop-in lp / lpr behavior */
    if (strcmp(prog, "lp") == 0 || strcmp(prog, "lpr") == 0) {
        const char *file = NULL;
        const char *title = "Command-Line Print Job";
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "-P") == 0) {
                i++;
            } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "-J") == 0 || strcmp(argv[i], "-T") == 0) {
                if (i + 1 < argc) title = argv[++i];
            } else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "-#") == 0) {
                if (i + 1 < argc) {
                    char cmd[64], rep[64];
                    snprintf(cmd, sizeof(cmd), "SET_PREF copies=%d", atoi(argv[++i]));
                    send_ctrl_cmd(cmd, rep, sizeof(rep));
                }
            } else if (strcmp(argv[i], "-o") == 0) {
                i++;
            } else if (argv[i][0] != '-') {
                file = argv[i];
            }
        }
        if (!file) {
            char tmp_path[128];
            snprintf(tmp_path, sizeof(tmp_path), "/tmp/lbp2900-stdin-%d.ps", getpid());
            FILE *fp = fopen(tmp_path, "wb");
            if (!fp) return 1;
            uint8_t buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0) fwrite(buf, 1, n, fp);
            fclose(fp);
            int rc = submit_file_via_ctrl(tmp_path, title);
            unlink(tmp_path);
            return rc;
        } else {
            return submit_file_via_ctrl(file, title);
        }
    }

    if (argc < 2) {
        print_usage(prog);
        return 0;
    }

    const char *sub = argv[1];
    char reply[2048];

    if (strcmp(sub, "status") == 0) {
        if (send_ctrl_cmd("STATUS", reply, sizeof(reply)) < 0) {
            fprintf(stderr, "Error: Cannot connect to lbp2900d at %s\n", LBP2900_CTRL_SOCK);
            return 1;
        }
        printf("%s", reply);
        return 0;
    } else if (strcmp(sub, "queue") == 0) {
        if (send_ctrl_cmd("QUEUE", reply, sizeof(reply)) < 0) return 1;
        printf("%s", reply);
        return 0;
    } else if (strcmp(sub, "purge") == 0) {
        if (send_ctrl_cmd("PURGE_QUEUE", reply, sizeof(reply)) < 0) return 1;
        printf("%s", reply);
        return 0;
    } else if (strcmp(sub, "prefs") == 0) {
        if (send_ctrl_cmd("GET_PREFS", reply, sizeof(reply)) < 0) {
            fprintf(stderr, "Error: Cannot connect to lbp2900d\n");
            return 1;
        }
        printf("%s", reply);
        return 0;
    } else if (strcmp(sub, "set") == 0) {
        for (int i = 2; i < argc; i++) {
            char cmd[512];
            snprintf(cmd, sizeof(cmd), "SET_PREF %s", argv[i]);
            if (send_ctrl_cmd(cmd, reply, sizeof(reply)) < 0) return 1;
        }
        printf("OK\n");
        return 0;
    } else if (strcmp(sub, "print") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s print <file> [title]\n", prog);
            return 1;
        }
        const char *file = argv[2];
        const char *title = (argc >= 4) ? argv[3] : file;
        return submit_file_via_ctrl(file, title);
    } else if (strcmp(sub, "test-page") == 0) {
        int pages = (argc >= 3) ? atoi(argv[2]) : 1;
        if (pages < 1) pages = 1;
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "TEST_PAGE %d", pages);
        if (send_ctrl_cmd(cmd, reply, sizeof(reply)) < 0) return 1;
        printf("Triggered %d-page test print: %s", pages, reply);
        return 0;
    } else if (strcmp(sub, "pause") == 0) {
        send_ctrl_cmd("PAUSE 1", reply, sizeof(reply));
        printf("%s", reply);
        return 0;
    } else if (strcmp(sub, "resume") == 0) {
        send_ctrl_cmd("PAUSE 0", reply, sizeof(reply));
        printf("%s", reply);
        return 0;
    } else if (strcmp(sub, "resume-paper") == 0) {
        send_ctrl_cmd("RESUME_PAPER", reply, sizeof(reply));
        printf("%s", reply);
        return 0;
    } else if (strcmp(sub, "cancel") == 0) {
        send_ctrl_cmd("CANCEL", reply, sizeof(reply));
        printf("%s", reply);
        return 0;
    } else if (strcmp(sub, "clean") == 0) {
        send_ctrl_cmd("CLEAN", reply, sizeof(reply));
        printf("%s", reply);
        return 0;
    } else if (strcmp(sub, "sim-disconnect") == 0) {
        int val = (argc >= 3) ? atoi(argv[2]) : 1;
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "SIMULATE_DISCONNECT %d", val);
        if (send_ctrl_cmd(cmd, reply, sizeof(reply)) < 0) return 1;
        printf("%s", reply);
        return 0;
    } else if (strcmp(sub, "sim-fault") == 0) {
        const char *ft = (argc >= 3) ? argv[2] : "clear";
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "SIMULATE_FAULT %s", ft);
        if (send_ctrl_cmd(cmd, reply, sizeof(reply)) < 0) return 1;
        printf("%s", reply);
        return 0;
    } else if (strcmp(sub, "ui") == 0) {
        execl("./bin/lbp2900-ui", "lbp2900-ui", NULL);
        perror("execl");
        return 1;
    } else {
        print_usage(prog);
        return 1;
    }
}
