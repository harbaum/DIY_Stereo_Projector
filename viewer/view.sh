#!/bin/bash
if [ $# != 1 ]; then
   echo "Please give an image name as argument"
   exit -1
fi

# non-4:3 images are cropped, so the result is full screen
convert $1 -resize 640x480^ -gravity center -extent 640x480 -depth 8 BGR:/tmp/image.raw ; ./t20 /tmp/image.raw
rm /tmp/image.raw
