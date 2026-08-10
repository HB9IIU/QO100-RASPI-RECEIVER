#!/usr/bin/env bash

# Change working directory to this directory to pick up htdocs dir at runtime
source_dir="$(cd $(dirname "${BASH_SOURCE[0]}") && pwd)";
cd "${source_dir}";

# UDP Transport Stream Output
TS_HOST=127.0.0.1
TS_PORT=9002

# UDP Status Output
STATUS_HOST=127.0.0.1
STATUS_PORT=9003

# Webpage controls
WEB_PORT=9004

# Polarisation (ie. LNB Voltage)
POL=v

# Timeout without TS before receiver reset (milliseconds)
TS_TIMEOUT=5000

# Receiver configuration (Frequency in kHz, Symbolrate in ks/s)
FREQ=2395000
SR=2000

./longmynd \
	-i ${TS_HOST} ${TS_PORT} \
	-I ${STATUS_HOST} ${STATUS_PORT} \
	-W ${WEB_PORT} \
	-p ${POL} \
	-r ${TS_TIMEOUT} \
	${FREQ} ${SR}

