#
# Makefile
#

APP = t20

all: $(APP)

clean:
	rm -f $(APP)

$(APP): $(APP).c
	$(CC) -o $@ $(APP).c `pkg-config --libs --cflags libusb-1.0`
