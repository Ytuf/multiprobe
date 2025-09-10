#!/bin/sh

# ========================================== 
# Internal script to generate datasheet
# Usage: See build.sh to run inside docker container
# ==========================================
cd /docs
asciidoctor-pdf -r asciidoctor -a pdf-theme=picolemon-theme.yml -r asciidoctor-diagram datasheet.adoc --out-file datasheet.pdf -a pdf-fontsdir=./fonts

