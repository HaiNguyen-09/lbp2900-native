CC = gcc
CFLAGS = -O3 -Wall -Wextra -Wno-unused-parameter -pthread
USB_SO = /lib/x86_64-linux-gnu/libusb-1.0.so.0
GTK_LIBS = /lib/x86_64-linux-gnu/libadwaita-1.so.0 \
           /lib/x86_64-linux-gnu/libgtk-4.so.1 \
           /lib/x86_64-linux-gnu/libgobject-2.0.so.0 \
           /lib/x86_64-linux-gnu/libglib-2.0.so.0 \
           /lib/x86_64-linux-gnu/libcairo.so.2 -lm

BIN_DIR = bin

all: $(BIN_DIR)/lbp2900d $(BIN_DIR)/lbp2900-ui $(BIN_DIR)/lbp2900ctl $(BIN_DIR)/verify_harness $(BIN_DIR)/verify_asan symlinks

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/lbp2900d: src/lbp2900d.c src/capt_usb.c src/renderer.c src/hiscoa.c src/capt_defs.h src/capt_usb.h src/renderer.h | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ src/lbp2900d.c src/capt_usb.c src/renderer.c src/hiscoa.c $(USB_SO) -lcups -lcupsimage -lm

$(BIN_DIR)/lbp2900-ui: src/lbp2900_ui.c src/capt_defs.h | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ src/lbp2900_ui.c $(GTK_LIBS)

$(BIN_DIR)/lbp2900ctl: src/lbp2900_cli.c src/capt_defs.h | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ src/lbp2900_cli.c -lcups

$(BIN_DIR)/verify_harness: tests/verify_harness.c src/hiscoa.c src/renderer.c src/capt_defs.h src/renderer.h | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ tests/verify_harness.c src/hiscoa.c src/renderer.c -lcups -lcupsimage -lm

$(BIN_DIR)/verify_asan: tests/verify_harness.c src/hiscoa.c src/renderer.c src/capt_defs.h src/renderer.h | $(BIN_DIR)
	$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -pthread -o $@ tests/verify_harness.c src/hiscoa.c src/renderer.c -lcups -lcupsimage -lm

symlinks: $(BIN_DIR)/lbp2900ctl
	ln -sf lbp2900ctl $(BIN_DIR)/lp
	ln -sf lbp2900ctl $(BIN_DIR)/lpr
	ln -sf lbp2900ctl $(BIN_DIR)/lpstat
	ln -sf lbp2900ctl $(BIN_DIR)/lpq
	ln -sf lbp2900ctl $(BIN_DIR)/lprm
	ln -sf lbp2900ctl $(BIN_DIR)/cancel

clean:
	rm -rf $(BIN_DIR)

.PHONY: all symlinks clean
