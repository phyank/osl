#!/bin/bash

rm -rf server
rm -rf client
mkdir server
mkdir client
cp cmake-build-debug/oslab4 server/sv
cp cmake-build-debug/oslab4 client/cl
