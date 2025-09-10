#!/bin/sh
# ========================================== 
# Script to build datasheet pdfs
# ==========================================
docker build -t "docbuilder:Dockerfile" .
docker run -v .:/docs -it docker.io/library/docbuilder:Dockerfile "/docs/gen_datasheet.docker.sh"
