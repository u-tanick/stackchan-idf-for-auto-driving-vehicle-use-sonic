IDF_PATH    ?= $(HOME)/esp-idf/5.4.2
IDF_EXPORTS  = $(IDF_PATH)/export.sh
TARGET      ?= esp32s3

# Serial port for flash / monitor / erase-flash. Leave unset to let idf.py
# auto-detect; pass on the command line (`make flash PORT=/dev/ttyACM0`) or
# pin in Makefile.local.
PORT        ?=
IDFPY_PORT   = $(if $(PORT),-p $(PORT))

# Allow developers to keep local overrides in an untracked file.
-include Makefile.local

# Build sdkconfig defaults chains.
#   - sdkconfig.defaults                  (committed, common)
#   - sdkconfig.defaults.<target>         (committed, per-target — picked up
#     automatically by IDF, but listed explicitly here so we can chain
#     sdkconfig.defaults.local after it)
#   - sdkconfig.defaults.local            (gitignored, host-specific overrides;
#     the last entry wins for duplicate keys so local values trump committed
#     defaults)
SDKCONFIG_DEFAULTS_HW   = sdkconfig.defaults
ifneq (,$(wildcard sdkconfig.defaults.$(TARGET)))
SDKCONFIG_DEFAULTS_HW  := $(SDKCONFIG_DEFAULTS_HW);sdkconfig.defaults.$(TARGET)
endif
ifneq (,$(wildcard sdkconfig.defaults.local))
SDKCONFIG_DEFAULTS_HW  := $(SDKCONFIG_DEFAULTS_HW);sdkconfig.defaults.local
endif

.PHONY: build clean set-target flash flash-monitor monitor monitor-log erase-flash \
        build-docker docker-shell docker-clean \
        audio-cli audio-play audio-test

# Capture serial output non-interactively (for CI / agent contexts where
# idf.py monitor refuses to attach without a TTY). SECONDS defaults to 8.
MONITOR_LOG_SECONDS ?= 8

# --- Target builds ----------------------------------------------------------

set-target:
	bash -c "source $(IDF_EXPORTS) && idf.py -DSDKCONFIG_DEFAULTS='$(SDKCONFIG_DEFAULTS_HW)' set-target $(TARGET)"

build:
	bash -c "source $(IDF_EXPORTS) && idf.py -DSDKCONFIG_DEFAULTS='$(SDKCONFIG_DEFAULTS_HW)' build"

flash:
	bash -c "source $(IDF_EXPORTS) && idf.py $(IDFPY_PORT) flash"

monitor:
	bash -c "source $(IDF_EXPORTS) && idf.py $(IDFPY_PORT) monitor"

# Non-interactive log capture: resets the target and reads stdout for
# MONITOR_LOG_SECONDS seconds. Requires pyserial.
monitor-log:
	python3 tools/monitor_log.py --port $(if $(PORT),$(PORT),/dev/ttyACM0) --seconds $(MONITOR_LOG_SECONDS)

# Convenience: flash + monitor in a single idf.py invocation (faster than
# `make flash monitor` because the IDF env is sourced once).
flash-monitor:
	bash -c "source $(IDF_EXPORTS) && idf.py $(IDFPY_PORT) flash monitor"

erase-flash:
	bash -c "source $(IDF_EXPORTS) && idf.py $(IDFPY_PORT) erase-flash"

clean:
	bash -c "source $(IDF_EXPORTS) && idf.py fullclean"

# --- Docker build (mirrors CI) ----------------------------------------------
DOCKER_IMAGE         ?= espressif/idf:release-v6.0
DOCKER_WORKDIR       ?= /work
DOCKER_BUILD_DIR     ?= build-docker
DOCKER_SDKCONFIG     ?= sdkconfig.docker
DOCKER_TARGET_CHIP   ?= esp32s3

define docker-run
	docker run --rm -t \
		-u $$(id -u):$$(id -g) \
		-v "$(CURDIR):$(DOCKER_WORKDIR)" \
		-w $(DOCKER_WORKDIR) \
		-e HOME=/tmp \
		-e CI=true \
		$(DOCKER_IMAGE) \
		bash -c '\
			git config --global --add safe.directory "$(DOCKER_WORKDIR)" && \
			. "$$IDF_PATH/export.sh" && \
			$(1)'
endef

build-docker:
	$(call docker-run, \
		idf.py -B $(DOCKER_BUILD_DIR) -DSDKCONFIG=$(DOCKER_SDKCONFIG) \
		       -DSDKCONFIG_DEFAULTS=sdkconfig.defaults \
		       set-target $(DOCKER_TARGET_CHIP) && \
		idf.py -B $(DOCKER_BUILD_DIR) -DSDKCONFIG=$(DOCKER_SDKCONFIG) \
		       -DSDKCONFIG_DEFAULTS=sdkconfig.defaults build)

docker-clean:
	$(call docker-run, \
		idf.py -B $(DOCKER_BUILD_DIR) -DSDKCONFIG=$(DOCKER_SDKCONFIG) \
		       fullclean)
	@rm -f $(DOCKER_SDKCONFIG)

# --- BLE audio streaming CLI (tools/audio-cli) ------------------------------
#
# Wraps the Rust CLI for end-to-end BLE audio test. The combined target
# starts the device serial log capture in the background so the AAC
# decoder diagnostics line up with the CLI's send progress in the same
# transcript.

AUDIO_CLI_BIN  ?= tools/audio-cli/target/release/audio-cli
AUDIO_DEVICE   ?= Stackchan-E2604E
AUDIO_FILE     ?= tools/audio-cli/out.aac
AUDIO_RATE     ?= 100
AUDIO_FLUSH    ?= 16
AUDIO_CHUNK    ?= 250
AUDIO_LOG_SEC  ?= 120

audio-cli:
	cd tools/audio-cli && cargo build --release

audio-play: audio-cli
	$(AUDIO_CLI_BIN) \
	    --device "$(AUDIO_DEVICE)" \
	    --file "$(AUDIO_FILE)" \
	    --chunk-size $(AUDIO_CHUNK) \
	    --rate-kbps $(AUDIO_RATE) \
	    --flush-every $(AUDIO_FLUSH)

# Parallel: serial log capture in the background while audio-play runs in
# the foreground. The device-side `audio-stream:` and `cfg-gatt:` lines
# interleave with the CLI's "sent N / total B" progress so the failure
# mode is obvious in one pane.
#
# The CLI is held back until the conv-task reports "session ready" so the
# streaming test runs against the real BLE/Wi-Fi radio-contention picture
# the conversation backend creates, not the wide-open BLE we'd see
# pre-association.
AUDIO_TEST_READY_TIMEOUT ?= 60
audio-test: audio-cli
	@echo "=== launching serial log capture (PORT=$(if $(PORT),$(PORT),/dev/ttyACM0), $(AUDIO_LOG_SEC) s) ==="
	@python3 tools/monitor_log.py \
	    --port $(if $(PORT),$(PORT),/dev/ttyACM0) \
	    --seconds $(AUDIO_LOG_SEC) > /tmp/audio-test-monitor.log 2>&1 & \
	    MONITOR_PID=$$!; \
	    echo "=== waiting for conv-task: session ready (≤$(AUDIO_TEST_READY_TIMEOUT) s) ==="; \
	    for i in $$(seq 1 $(AUDIO_TEST_READY_TIMEOUT)); do \
	        if grep -q "conv-task: session ready" /tmp/audio-test-monitor.log 2>/dev/null; then \
	            echo "conv-task ready at +$${i}s"; \
	            break; \
	        fi; \
	        sleep 1; \
	    done; \
	    if ! grep -q "conv-task: session ready" /tmp/audio-test-monitor.log 2>/dev/null; then \
	        echo "WARN: conv-task: session ready did not show within $(AUDIO_TEST_READY_TIMEOUT)s — running test anyway"; \
	    fi; \
	    echo "=== streaming $(AUDIO_FILE) to $(AUDIO_DEVICE) ==="; \
	    $(AUDIO_CLI_BIN) \
	        --device "$(AUDIO_DEVICE)" \
	        --file "$(AUDIO_FILE)" \
	        --chunk-size $(AUDIO_CHUNK) \
	        --rate-kbps $(AUDIO_RATE) \
	        --flush-every $(AUDIO_FLUSH); \
	    echo "=== waiting for monitor to finish ==="; \
	    wait $$MONITOR_PID; \
	    echo "=== device log (tail) ==="; \
	    tail -n 80 /tmp/audio-test-monitor.log

docker-shell:
	docker run --rm -it \
		-u $$(id -u):$$(id -g) \
		-v "$(CURDIR):$(DOCKER_WORKDIR)" \
		-w $(DOCKER_WORKDIR) \
		-e HOME=/tmp \
		$(DOCKER_IMAGE) \
		bash -c '\
			git config --global --add safe.directory "$(DOCKER_WORKDIR)" && \
			. "$$IDF_PATH/export.sh" && \
			exec bash'
