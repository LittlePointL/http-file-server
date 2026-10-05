#!/usr/bin/env bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

APP_NAME=fileserver

PORT=9876

ROOT_DIR=root_dir


$SCRIPT_DIR/$APP_NAME -p $PORT -r $ROOT_DIR

