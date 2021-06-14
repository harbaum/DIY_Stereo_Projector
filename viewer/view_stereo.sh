#!/bin/bash
if [ $# != 1 ]; then
   echo "Please give an image name as argument"
   exit -1
fi

# cut into left and right half
convert $1 -crop 50%x100% +repage -resize 640x480^ -gravity center -extent 640x480 -depth 8 BGR:/tmp/image_%d.raw
./t20 /tmp/image_0.raw /tmp/image_1.raw
rm /tmp/image_?.raw
