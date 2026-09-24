# powerprofile
# See LICENSE file for copyright and license details.

VERSION  ?= 0.1.0
CC       ?= cc

CFLAGS ?= -Os
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wpointer-arith -Wshadow -Wstrict-prototypes \
	-Wmissing-prototypes -Wold-style-definition -Wformat=2 -Wconversion -Wsign-conversion

CPPFLAGS += -MMD -MP -DVERSION=\"${VERSION}\"

PKG        := x11 xrandr
PKG_CONFIG ?= pkg-config
CPPFLAGS   += $(shell $(PKG_CONFIG) --cflags $(PKG) 2>/dev/null)
X_LIBS     := $(shell $(PKG_CONFIG) --libs $(PKG) 2>/dev/null)
ifeq ($(X_LIBS),)
X_LIBS := -lX11 -lXrandr
endif

DEBUG_CFLAGS  := -g3 -O0 -fsanitize=address,undefined -fno-omit-frame-pointer
DEBUG_LDFLAGS := -fsanitize=address,leak,undefined

# -fanalyzer is GCC-only; enable it only when CC is gcc so `make debug`
# still works under clang.
ifneq (,$(findstring gcc,$(shell $(CC) --version 2>/dev/null)))
DEBUG_CFLAGS += -fanalyzer
endif

PREFIX      ?= /usr/local
MANPREFIX   ?= ${PREFIX}/share/man
SYSCONFDIR  ?= /etc
SYSTEMDDIR  ?= ${PREFIX}/lib/systemd/system
UDEVDIR     ?= ${PREFIX}/lib/udev/rules.d
BASHCOMPDIR ?= ${PREFIX}/share/bash-completion/completions
ZSHCOMPDIR  ?= ${PREFIX}/share/zsh/site-functions
FISHCOMPDIR ?= ${PREFIX}/share/fish/vendor_completions.d

SRCDIR   := src
BUILDDIR := build
BINDIR   := $(BUILDDIR)/bin
OBJDIR   := $(BUILDDIR)/obj
DOCDIR   := docs

BIN    := powerprofile
HELPER := powerprofile-x

CLI_SRCS    := powerprofile.c ec.c ini.c utils.c
HELPER_SRCS := powerprofile-x.c ini.c utils.c

CLI_OBJS    := $(CLI_SRCS:%.c=$(OBJDIR)/%.o)
HELPER_OBJS := $(HELPER_SRCS:%.c=$(OBJDIR)/%.o)
OBJS        := $(sort $(CLI_OBJS) $(HELPER_OBJS))
DEPS        := $(OBJS:.o=.d)

TARGET        := $(BINDIR)/$(BIN)
TARGET_HELPER := $(BINDIR)/$(HELPER)

COLOR  ?= 1
PRINTF ?= printf

ifeq ($(COLOR),0)
COLOR_RESET  :=
COLOR_GREEN  :=
COLOR_YELLOW :=
COLOR_BLUE   :=
COLOR_CYAN   :=
else
COLOR_RESET  := \033[0m
COLOR_GREEN  := \033[1;32m
COLOR_YELLOW := \033[1;33m
COLOR_BLUE   := \033[1;34m
COLOR_CYAN   := \033[1;36m
endif

all: $(TARGET) $(TARGET_HELPER)

debug: CFLAGS += $(DEBUG_CFLAGS)
debug: LDFLAGS += $(DEBUG_LDFLAGS)
debug: clean all

$(TARGET): $(CLI_OBJS) | $(BINDIR)
	@$(PRINTF) "$(COLOR_GREEN)Linking:$(COLOR_RESET) %s\n" "$@"
	@$(CC) $(LDFLAGS) -o $@ $(CLI_OBJS)

$(TARGET_HELPER): $(HELPER_OBJS) | $(BINDIR)
	@$(PRINTF) "$(COLOR_GREEN)Linking:$(COLOR_RESET) %s\n" "$@"
	@$(CC) $(LDFLAGS) -o $@ $(HELPER_OBJS) $(X_LIBS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	@$(PRINTF) "$(COLOR_BLUE)Compiling:$(COLOR_RESET) %s\n" "$@"
	@$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BINDIR) $(OBJDIR):
	@mkdir -p $@

clean:
	@$(PRINTF) "$(COLOR_YELLOW)Cleaning:$(COLOR_RESET) %s\n" "$(BUILDDIR)"
	@rm -rf $(BUILDDIR)

install: all
	@$(PRINTF) "$(COLOR_CYAN)Installing $(BIN) and $(HELPER) at:$(COLOR_RESET) %s\n" "$(DESTDIR)$(PREFIX)/bin"
	@install -d $(DESTDIR)$(PREFIX)/bin
	@install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	@install -m 755 $(TARGET_HELPER) $(DESTDIR)$(PREFIX)/bin/$(HELPER)
	@install -d $(DESTDIR)$(MANPREFIX)/man1 $(DESTDIR)$(MANPREFIX)/man5
	@sed "s/VERSION/$(VERSION)/g" < $(DOCDIR)/$(BIN).1 > $(DESTDIR)$(MANPREFIX)/man1/$(BIN).1
	@sed "s/VERSION/$(VERSION)/g" < $(DOCDIR)/$(HELPER).1 > $(DESTDIR)$(MANPREFIX)/man1/$(HELPER).1
	@sed "s/VERSION/$(VERSION)/g" < $(DOCDIR)/$(BIN).conf.5 > $(DESTDIR)$(MANPREFIX)/man5/$(BIN).conf.5
	@chmod 644 $(DESTDIR)$(MANPREFIX)/man1/$(BIN).1 $(DESTDIR)$(MANPREFIX)/man1/$(HELPER).1 \
		$(DESTDIR)$(MANPREFIX)/man5/$(BIN).conf.5
	@$(PRINTF) "$(COLOR_CYAN)Installing systemd unit and udev rule at:$(COLOR_RESET) %s\n" "$(DESTDIR)$(SYSTEMDDIR)"
	@install -d $(DESTDIR)$(SYSTEMDDIR) $(DESTDIR)$(UDEVDIR)
	@sed "s|@BINDIR@|$(PREFIX)/bin|g" < systemd/$(BIN).service \
		> $(DESTDIR)$(SYSTEMDDIR)/$(BIN).service
	@sed "s|@BINDIR@|$(PREFIX)/bin|g" < udev/99-$(BIN).rules \
		> $(DESTDIR)$(UDEVDIR)/99-$(BIN).rules
	@chmod 644 $(DESTDIR)$(SYSTEMDDIR)/$(BIN).service $(DESTDIR)$(UDEVDIR)/99-$(BIN).rules
	@$(PRINTF) "$(COLOR_CYAN)Installing config at:$(COLOR_RESET) %s\n" "$(DESTDIR)$(SYSCONFDIR)/$(BIN).conf"
	@test -e $(DESTDIR)$(SYSCONFDIR)/$(BIN).conf \
		|| install -Dm644 conf/$(BIN).conf $(DESTDIR)$(SYSCONFDIR)/$(BIN).conf
	@install -Dm644 completions/$(BIN).bash $(DESTDIR)$(BASHCOMPDIR)/$(BIN)
	@install -Dm644 completions/$(HELPER).bash $(DESTDIR)$(BASHCOMPDIR)/$(HELPER)
	@install -Dm644 completions/_$(BIN)     $(DESTDIR)$(ZSHCOMPDIR)/_$(BIN)
	@install -Dm644 completions/_$(HELPER)  $(DESTDIR)$(ZSHCOMPDIR)/_$(HELPER)
	@install -Dm644 completions/$(BIN).fish $(DESTDIR)$(FISHCOMPDIR)/$(BIN).fish
	@install -Dm644 completions/$(HELPER).fish $(DESTDIR)$(FISHCOMPDIR)/$(HELPER).fish
	@$(PRINTF) "\n$(COLOR_GREEN)Installed.$(COLOR_RESET) Now edit $(SYSCONFDIR)/$(BIN).conf and run:\n"
	@$(PRINTF) "    sudo systemctl daemon-reload\n"
	@$(PRINTF) "    sudo systemctl enable $(BIN)\n"
	@$(PRINTF) "    sudo udevadm control --reload\n"

# the config file is left in place
uninstall:
	@$(PRINTF) "$(COLOR_CYAN)Uninstalling $(BIN) from:$(COLOR_RESET) %s\n" "$(DESTDIR)$(PREFIX)/bin"
	@rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN) $(DESTDIR)$(PREFIX)/bin/$(HELPER)
	@rm -f $(DESTDIR)$(MANPREFIX)/man1/$(BIN).1 $(DESTDIR)$(MANPREFIX)/man1/$(HELPER).1 \
		$(DESTDIR)$(MANPREFIX)/man5/$(BIN).conf.5
	@rm -f $(DESTDIR)$(SYSTEMDDIR)/$(BIN).service $(DESTDIR)$(UDEVDIR)/99-$(BIN).rules
	@rm -f $(DESTDIR)$(BASHCOMPDIR)/$(BIN) $(DESTDIR)$(BASHCOMPDIR)/$(HELPER) \
	       $(DESTDIR)$(ZSHCOMPDIR)/_$(BIN) $(DESTDIR)$(ZSHCOMPDIR)/_$(HELPER) \
	       $(DESTDIR)$(FISHCOMPDIR)/$(BIN).fish $(DESTDIR)$(FISHCOMPDIR)/$(HELPER).fish

TESTSDIR := $(SRCDIR)/tests
TESTDIR  := $(BUILDDIR)/tests
TESTROOT := $(TESTDIR)/root
TESTSAN  := -g3 -O0 -fsanitize=address,undefined -fno-omit-frame-pointer
TESTCFLAGS := $(filter-out -Os,$(CFLAGS)) $(TESTSAN) $(DEBUG_LDFLAGS) -I$(SRCDIR)

TEST_EC     := $(TESTDIR)/test-ec
TEST_EC_SRC := $(TESTSDIR)/test-ec.c $(SRCDIR)/ec.c $(SRCDIR)/utils.c

# A synthetic sysfs tree, so the validation rules can be tested on any
# machine rather than only on a supported MSI laptop. Rebuilt on every run:
# the tests write to it, so a second run must not start from the first one's
# leftovers.
$(TESTROOT):
	@rm -rf $(TESTROOT)
	@mkdir -p $(TESTROOT)/platform/msi-ec/cpu $(TESTROOT)/platform/msi-ec/gpu \
		$(TESTROOT)/platform/msi-ec/leds/platform::mute \
		$(TESTROOT)/platform/msi-ec/leds/msiacpi::kbd_backlight \
		$(TESTROOT)/supply/BAT1
	@printf 'comfort\n'                     > $(TESTROOT)/platform/msi-ec/shift_mode
	@printf 'eco\ncomfort\nsport\nturbo\n'  > $(TESTROOT)/platform/msi-ec/available_shift_modes
	@printf 'auto\n'                        > $(TESTROOT)/platform/msi-ec/fan_mode
	@printf 'auto\nsilent\nadvanced\n'      > $(TESTROOT)/platform/msi-ec/available_fan_modes
	@printf 'off\n'                         > $(TESTROOT)/platform/msi-ec/cooler_boost
	@printf 'off\n'                         > $(TESTROOT)/platform/msi-ec/super_battery
	@printf 'left\n'                        > $(TESTROOT)/platform/msi-ec/fn_key
	@printf '55\n'                          > $(TESTROOT)/platform/msi-ec/cpu/realtime_temperature
	@printf '2200\n'                        > $(TESTROOT)/platform/msi-ec/cpu/realtime_fan_speed
	@printf '60\n'                          > $(TESTROOT)/platform/msi-ec/gpu/realtime_temperature
	@printf '0\n'                           > $(TESTROOT)/platform/msi-ec/gpu/realtime_fan_speed
	@printf '0\n'                           > $(TESTROOT)/platform/msi-ec/leds/platform::mute/brightness
	@printf '1\n'                           > $(TESTROOT)/platform/msi-ec/leds/platform::mute/max_brightness
	@printf '0\n'                           > $(TESTROOT)/platform/msi-ec/leds/msiacpi::kbd_backlight/brightness
	@printf '3\n'                           > $(TESTROOT)/platform/msi-ec/leds/msiacpi::kbd_backlight/max_brightness
	@printf '60\n'                          > $(TESTROOT)/supply/BAT1/charge_control_start_threshold
	@printf '100\n'                         > $(TESTROOT)/supply/BAT1/charge_control_end_threshold
	@printf '87\n'                          > $(TESTROOT)/supply/BAT1/capacity
	@printf 'Discharging\n'                 > $(TESTROOT)/supply/BAT1/status

test: $(TESTROOT)
	@$(PRINTF) "$(COLOR_BLUE)Testing:$(COLOR_RESET) %s\n" "$(TEST_EC)"
	@$(CC) $(TESTCFLAGS) \
		-DEC_PLATFORM_DIR='"$(TESTROOT)/platform/msi-ec"' \
		-DEC_SUPPLY_DIR='"$(TESTROOT)/supply"' \
		-DTESTROOT='"$(TESTROOT)"' \
		-o $(TEST_EC) $(TEST_EC_SRC)
	@$(TEST_EC)
	@$(PRINTF) "$(COLOR_BLUE)Testing:$(COLOR_RESET) %s\n" "$(TESTSDIR)/test-profile.sh"
	@CC="$(CC)" CFLAGS="$(TESTCFLAGS)" sh $(TESTSDIR)/test-profile.sh $(TESTDIR)

-include $(DEPS)

.PHONY: all debug clean install uninstall test $(TESTROOT)
