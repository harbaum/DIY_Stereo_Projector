/*
  t20.c - aiptek t20 test and benchmark application

  Till Harbaum <till@harbaum.org>
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#include <libusb.h>
#include <endian.h>

#if __BYTE_ORDER != __LITTLE_ENDIAN
#warning "Code has only been tested on little endian!"
#warning "Please report big endian results to till@harbaum.org."
#define htole24(x) \
  ((((((x) & 0xff0000) >>  16) | ((x) & 0xff00) | (((x) & 0xff) << 16))
#else
#define htole24(x) (x)
#endif


/* the vendor and product id of the aiptek t20 */
#define USB_VID  0x08ca
#define USB_PID  0x2137

/* buffer holds one complete image */
#define BUFFER_SIZE  (640*480*3)

/* endpoints being used */
#define EP_CMDIN     0x81   /* command in */
#define EP_IMGOUT    0x02   /* image data out */
#define EP_CMDOUT    0x03   /* command out */

#define TIMEOUT   100   /* 100 ms usb timeout */

/* structures describing the different packet types */
/* caution, this currently only works on little endian!!! */

/* EP3 command 0x02: write 8 bit register */
typedef struct {
  uint8_t cmd;
  uint16_t length;   // always 1 (big endian!)
  uint16_t reg;      // register index (big endian!)
  uint8_t value;     // value to write
} __attribute__((packed)) write_reg_t;

/* EP3 command 0x04: write byte sequence */
typedef struct {
  uint8_t cmd;
  uint16_t length;   // number of bytes following
  uint8_t data[0];
} __attribute__((packed)) write_seq_t;

/* EP3 command 0x05: read 8 bit register */
typedef struct {
  uint8_t cmd;
  uint16_t reg;      // register index
} __attribute__((packed)) read_reg_t;

typedef struct {
  uint16_t reg;
  uint8_t val;
} __attribute__((packed)) reg_t;

/* EP3 command 0x22: write register sequence */
typedef struct {
  uint8_t cmd;
  uint8_t length;   // number of registers following
  reg_t data[0];
} __attribute__((packed)) write_reg_seq_t;

/* EP3 command 0x25: read sector */
typedef struct {
  uint8_t cmd;
  uint8_t offset;
  uint8_t unknown0;
  uint8_t unknown1;
} __attribute__((packed)) read_sec_t;

/* EP2 command 0x11: image data */
typedef struct {
  uint8_t cmd;
  uint32_t offset;
  struct {
    uint16_t width, height;
  } src, dest, unknown;
#if __BYTE_ORDER == __LITTLE_ENDIAN
  uint32_t flag:8; 
  uint32_t length:24; 
#else
  uint32_t length:24; 
  uint32_t flag:8; 
#endif
} __attribute__((packed)) image_hdr_t;

/* structure holding information necessary to communicate with t20 */
typedef struct {
  libusb_device_handle *handle;

  int in_transfer;   // async transfer state

  struct libusb_transfer *hdr_transfer;
  image_hdr_t hdr;
  struct libusb_transfer *transfer;
  char *buffer;
} t20_dev_t;

/* simple hex dump routine */
void dump(uint8_t *buf, int num) {
  register int i, n = 0, size;

  while (num > 0) {
    printf("%04x: ", n);

    size = num > 16 ? 16 : num;
    
    for (i = 0; i < size; i++)
      printf("%02x%s", buf[i], (i + 1) % 8 ? " " : "  ");
    for (i = size; i < 16; i++)
      printf("  %s", (i + 1) % 8 ? " " : "  ");
    
    for (i = 0; i < size; i++)
      printf("%1c", isprint(buf[i]) ? buf[i] : '.');
    printf("\n");
    
    buf  += size;
    num  -= size;
    n    += size;
  }
}

/* write one byte into a control register. for some odd reason this */
/* is the only command using big endian encoding */
int write_reg(libusb_device_handle *handle, uint16_t reg, uint8_t val) {
  int ret, tr;
  write_reg_t cmd = { 
    .cmd=0x02, .length=htobe16(1), .reg=htobe16(reg), .value=val };

  if((ret = libusb_bulk_transfer(handle, EP_CMDOUT, 
		 (void*)&cmd, sizeof(cmd), &tr, TIMEOUT)) < 0) {
    fprintf(stderr, "USB error write_reg(): %d\n", ret);
    return -1;								
  }

  return 0;
}

/* read one byte from a control register */
int read_reg(libusb_device_handle *handle, uint16_t reg) {
  int ret, tr;
  read_reg_t cmd = { .cmd=0x05, .reg=htole16(reg) };
  uint8_t val;

  /* write command */
  if((ret = libusb_bulk_transfer(handle, EP_CMDOUT, 
		 (void*)&cmd, sizeof(cmd), &tr, TIMEOUT)) < 0) {
    fprintf(stderr, "USB error read_reg() w: %d\n", ret);
    return -1;								
  }

  if((ret = libusb_bulk_transfer(handle, EP_CMDIN, 
				 &val, 1, &tr, TIMEOUT)) < 0) {
    fprintf(stderr, "USB error read_reg() r: %d\n", ret);
    return -1;								
  }

  return val;
}

/* trigger async transmission of image data */
int write_image_data(t20_dev_t *dev) {
  int ret;

  if((ret = libusb_submit_transfer(dev->transfer)) < 0) {
    fprintf(stderr, "USB error write_image_data(): %d\n", ret);
    return -1;
  }

  return 0;
} 

/* read a 640x480x3 BGR raw image and write it to the projector */
int write_image(t20_dev_t *dev, char *name) {
  int ret;

  /* load image into buffer if name was given, otherwise just clear buffer */
  FILE *file = name?fopen(name, "rb"):NULL;
  if(!file) 
    memset(dev->buffer, 0, 640*480*3);
  else {
    fread(dev->buffer, 640*480*3, 1, file);
    fclose(file);
  }

  /* wait for transfer buffer to become available */
  while(dev->in_transfer) 
    libusb_handle_events(NULL);

  dev->in_transfer++;
  if((ret = libusb_submit_transfer(dev->hdr_transfer)) < 0) {
    fprintf(stderr, "USB error write_image_data(): %d\n", ret);
    return -1;
  }

  write_image_data(dev);

  return 0;
}

int write_seq(libusb_device_handle *handle, const uint8_t *seq, int len) {
  int ret, tr;
  write_seq_t *hdr = malloc(sizeof(write_seq_t)+len);
  if(!hdr) {
    fprintf(stderr, "Out of memory in write_seq()\n");
    return -1;
  }

  /* setup header and append payload */
  hdr->cmd = 0x04;
  hdr->length = htole16(len);
  memcpy(hdr->data, seq, len);
  
  /* send complete packet */
  if((ret = libusb_bulk_transfer(handle, EP_CMDOUT, (void*)hdr, 
		 sizeof(write_seq_t)+len, &tr, TIMEOUT)) < 0) {
    fprintf(stderr, "USB error write_seq(): %d\n", ret);
    free(hdr);
    return -1;								
  }

  free(hdr);
  return 0;
}

int write_reg_seq(libusb_device_handle *handle, const reg_t *seq, int len) {
  int i, ret, tr;
  write_reg_seq_t *hdr = malloc(sizeof(write_reg_seq_t)+len*sizeof(reg_t));
  if(!hdr) {
    fprintf(stderr, "Out of memory in write_reg_seq()\n");
    return -1;
  }
  
  /* setup header and append payload */
  hdr->cmd = 0x22;
  hdr->length = len;

  /* register addresses are also big endian here */
  for(i=0;i<len;i++) {
    hdr->data[i].reg = htobe16(seq[i].reg);
    hdr->data[i].val = seq[i].val;
  }
  
  /* send complete packet */
  if((ret = libusb_bulk_transfer(handle, EP_CMDOUT, (void*)hdr, 
	 sizeof(write_reg_seq_t)+len*sizeof(reg_t), &tr, TIMEOUT)) < 0) {
    fprintf(stderr, "USB error write_reg_seq(): %d\n", ret);
    free(hdr);
    return -1;								
  }

  free(hdr);
  return 0;
}

int read_sector(libusb_device_handle *handle, uint8_t offset) {
  int ret, tr;
  uint8_t buffer[512];
  read_sec_t cmd = {
    .cmd = 0x25, .offset = offset, 
    .unknown0 = 0xff, .unknown1 = 0 
  };

  /* write command */
  if((ret = libusb_bulk_transfer(handle, EP_CMDOUT, 
		 (void*)&cmd, sizeof(cmd), &tr, TIMEOUT)) < 0) {
    fprintf(stderr, "USB error read_sector() w: %d\n", ret);
    return -1;
  }

  if((ret = libusb_bulk_transfer(handle, EP_CMDIN, 
				 buffer, 512, &tr, TIMEOUT)) < 0) {
    fprintf(stderr, "USB error read_sector() r: %d\n", ret);
    return -1;
  }

  dump(buffer, 512);

  return 0;
}

/* this is a rather odd command as it just sends 768 zero bytes */
int write_768x0(libusb_device_handle *handle) {
  int ret, tr;
  uint8_t cmd[769] = { 0x0a };
  memset(cmd+1, 0, 768);

  /* write command */
  if((ret = libusb_bulk_transfer(handle, EP_CMDOUT, 
		      (void*)cmd, sizeof(cmd), &tr, TIMEOUT)) < 0) {
    fprintf(stderr, "USB error write_768x0(): %d\n", ret);
    return -1;
  }

  return 0;
}

/* fc6c/fc6d seems to be some very basic 16 bit status register */
/* which is checked and written quite frequently during setup */
static void check_and_update_status(libusb_device_handle *handle, uint16_t val) {
  printf("0xfc6c.w: %04x\n", (read_reg(handle, 0xfc6c) << 8) | 
	                      read_reg(handle, 0xfc6d));

  write_reg(handle, 0xfc6c, val>>8);
  write_reg(handle, 0xfc6d, val&0x0ff);
}

/* this looks like some read test and the projector can obviously */
/* live without that */
static void perform_read_test(libusb_device_handle *handle) {

  /* read some 512 byte buffer */
  printf("0xfc6f: %02x\n", read_reg(handle, 0xfc6f));
  write_reg(handle, 0xfc6f, 0x04);
  read_sector(handle, 0x00);
  write_reg(handle, 0xfc6f, 0x00);

  /* read another 512 byte buffer */
  printf("0xfc6f: %02x\n", read_reg(handle, 0xfc6f));
  write_reg(handle, 0xfc6f, 0x04);
  read_sector(handle, 0x01);
  write_reg(handle, 0xfc6f, 0x00);
}

int t20_init(libusb_device_handle *handle) {

  /* various byte/register sequences downloaded during setup */
  const uint8_t seq0[] = {
    0x6a, 0x00, 0x6b, 0x0a,   0x6a, 0x01, 0x6b, 0x0a,
    0x6a, 0x02, 0x6b, 0x0a,   0x6a, 0x03, 0x6b, 0x0a,
    0x6a, 0x04, 0x6b, 0x0a,   0x6a, 0x05, 0x6b, 0x0a,
    0x6a, 0x06, 0x6b, 0x0a,   0x6a, 0x07, 0x6b, 0x0a,
    0x6a, 0x08, 0x6b, 0x0a,   0x6a, 0x09, 0x6b, 0x0a  };

  const uint8_t seq1[] = {
    0xf0, 0x04, 0xf1, 0x30,   0xf2, 0x01, 0xf3, 0x05, 
    0xf4, 0x07, 0xf6, 0x01,   0xf7, 0x03, 0xf5, 0x00,
    0xa3, 0x60, 0x4b, 0x0f }; 

  const uint8_t seq2[] = {
    0xa4, 0x37, 0xa3, 0x61,   0xa3, 0x60, 0x28, 0x01, 
    0x28, 0x00, 0x32, 0x00,   0x59, 0x01, 0x59, 0x00, 
    0x34, 0x00, 0xc4, 0x7f,   0xc5, 0x02, 0xc6, 0x32,
    0xc7, 0x34, 0xce, 0x1f,   0xcf, 0x03, 0xc8, 0xdf,
    0xc9, 0x01, 0xca, 0x0e,   0xcb, 0x0e, 0xcc, 0x0c,
    0xcd, 0x02 }; 

  const uint8_t seq3[] = {
    0x40, 0x00, 0x41, 0x40,   0x42, 0x00, 0x43, 0x40,
    0x36, 0x80, 0x37, 0x02,   0x38, 0xe0, 0x39, 0x01, 
    0x34, 0x0e }; 

  const reg_t seq4[] = {
    { 0xfb65, 0x01 }, { 0xfb62, 0x00 }, { 0xfb63, 0x08 }, { 0xfb64, 0x07 }, 
    { 0xfb65, 0x02 }, { 0xfb62, 0x00 }, { 0xfb63, 0x10 }, { 0xfb64, 0x0e }}; 

  write_seq(handle, seq0, sizeof(seq0));

  printf("0xfc01: %02x\n", read_reg(handle, 0xfc01));

  write_reg(handle, 0xfc6f, 0x00);

  write_768x0(handle);

  check_and_update_status(handle, 0x1001);
  check_and_update_status(handle, 0x3021);

  write_reg(handle, 0xfce0, 0x3f);
  write_reg(handle, 0xfce1, 0x00);
  write_reg(handle, 0xfce2, 0x80);

  //  perform_read_test(handle);

  write_reg(handle, 0xfc28, 0x01);
  write_reg(handle, 0xfc59, 0x01);
  write_reg(handle, 0xfc32, 0x00);
  write_reg(handle, 0xfc34, 0x00);
  write_reg(handle, 0xfcb0, 0x00);
  write_reg(handle, 0xfcb0, 0x20);
  write_reg(handle, 0xfc4b, 0x00);
  write_reg(handle, 0xfbff, 0x81);
  
  write_seq(handle, seq1, sizeof(seq1));

#if 0 // only in latest driver
  write_reg(handle, 0xfca8, 0x42);
  write_reg(handle, 0xfca8, 0x40);
#endif

  write_seq(handle, seq2, sizeof(seq2));
  write_seq(handle, seq3, sizeof(seq3));

  write_reg_seq(handle, seq4, sizeof(seq4)/sizeof(reg_t));

  write_reg(handle, 0xfc32, 0x00);

  write_reg(handle, 0xfcb4, 0x01);
  write_reg(handle, 0xfcb1, 0x00);
  write_reg(handle, 0xfcb2, 0x00);
  write_reg(handle, 0xfcb3, 0x00);

  write_reg(handle, 0xfb65, 0x11);
  write_reg(handle, 0xfb60, 0x00);

  write_reg(handle, 0xfcb5, 0x03);
  write_reg(handle, 0xfcb4, 0x10);
  write_reg(handle, 0xfb96, 0x11);
  write_reg(handle, 0xfcb0, 0x09);

  //  write_empty_bw(handle, 640, 480);

  check_and_update_status(handle, 0x3032);

  //  write_empty(handle, 28, 2);
  //  write_empty(handle, 640, 465);

  write_reg(handle, 0xfcb0, 0x20);
  write_reg(handle, 0xfc4b, 0x1c);
  
  check_and_update_status(handle, 0x3022);

  write_reg(handle, 0xfc28, 0x01);
  write_reg(handle, 0xfc59, 0x01);
  write_reg(handle, 0xfc32, 0x00);
  write_reg(handle, 0xfc34, 0x00);
  write_reg(handle, 0xfcb0, 0x00);
  write_reg(handle, 0xfcb0, 0x20);
  write_reg(handle, 0xfc4b, 0x00);
  write_reg(handle, 0xfbff, 0x81);

  write_seq(handle, seq1, sizeof(seq1));

#if 0 // only in latest driver
  write_reg(handle, 0xfca8, 0x42);
  write_reg(handle, 0xfca8, 0x40);
#endif

  write_seq(handle, seq2, sizeof(seq2));
  write_seq(handle, seq3, sizeof(seq3));

  write_reg(handle, 0xfc32, 0x00);

  write_reg_seq(handle, seq4, sizeof(seq4)/sizeof(reg_t));

  write_reg(handle, 0xfcb4, 0x01);
  write_reg(handle, 0xfcb1, 0x00);
  write_reg(handle, 0xfcb2, 0x00);
  write_reg(handle, 0xfcb3, 0x00);

  write_reg(handle, 0xfb65, 0x11);
  write_reg(handle, 0xfb60, 0x00);

  write_reg(handle, 0xfcb5, 0x03);
  write_reg(handle, 0xfcb4, 0x10);
  write_reg(handle, 0xfb96, 0x11);
  write_reg(handle, 0xfcb0, 0x09);

  //  write_empty_bw(handle, 640, 480);

  check_and_update_status(handle, 0x3032);

  puts("ok");

  return 0;
}

uint32_t get_msec(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_usec / 1000 + tv.tv_sec * 1000;
}

/* callback invoked by libusb for the header transfer */
void image_cb(struct libusb_transfer *transfer) {
  t20_dev_t *dev = (t20_dev_t*)(transfer->user_data);
  dev->in_transfer--;
}

/* scan usb subsystem for T20 devices and initialize them all */
static int find_all_t20(t20_dev_t **devs) {
  libusb_device **usb_devs;
  int ret, i, devices = 0;

  ssize_t cnt = libusb_get_device_list(NULL, &usb_devs);
  if (cnt < 0) return 0;

  *devs = NULL;

  for(i=0;usb_devs[i]!=NULL;i++) {
    struct libusb_device_descriptor desc;
    if(libusb_get_device_descriptor(usb_devs[i], &desc) == 0) {

      /* try to init all Aiptek T20 connected */
      if((desc.idVendor == USB_VID) && (desc.idProduct == USB_PID)) {
	libusb_device_handle *h;
	
	printf("Found device on bus %d device %d.\n", 
	       libusb_get_bus_number(usb_devs[i]), 
	       libusb_get_device_address(usb_devs[i]));
	
	/* open device */
	if((ret = libusb_open(usb_devs[i], &h)) != 0) 
	  fprintf(stderr, "USB error open(): %s\n", strerror(ret));
	else {
	  if ((ret = libusb_set_configuration(h, 1)) != 0) {
	    fprintf(stderr, "USB error set_configuration(): %s\n", strerror(ret));
	    libusb_close(h);
	    break;
	  }
	  
	  if ((ret = libusb_claim_interface(h, 0)) != 0) {
	    fprintf(stderr, "USB error claim_interface(): %s\n", strerror(ret)); 
	    libusb_close(h);
	    h = NULL;
	    break;
	  }
	  
	  t20_init(h);

	  /* allocate new entry for device */
	  *devs = realloc(*devs, (devices+1) * sizeof(t20_dev_t));
	  t20_dev_t *dev = *devs + devices;

	  /* create video buffer */
	  dev->buffer = malloc(BUFFER_SIZE);

	  /* create transfer buffers for async io and set them up */
	  dev->hdr_transfer = libusb_alloc_transfer(0);
	  libusb_fill_bulk_transfer(dev->hdr_transfer, h, 
			    EP_IMGOUT, NULL, sizeof(image_hdr_t), 
			    NULL, NULL, TIMEOUT);
	  dev->hdr_transfer->flags = LIBUSB_TRANSFER_SHORT_NOT_OK;

	  /* setup async image header */
	  dev->hdr.cmd = 0x11;
	  dev->hdr.offset = htole32(0);
	  dev->hdr.src.width = htole16(640);
	  dev->hdr.src.height = htole16(480);
	  dev->hdr.dest.width = htole16(640);
	  dev->hdr.dest.height = htole16(480);
	  dev->hdr.unknown.width = htole16(0x4000);
	  dev->hdr.unknown.height = htole16(0x4000);
	  dev->hdr.flag = 0x00;
	  dev->hdr.length = htole24(640*480*3);

	  /* create transfer buffer for async io and set it up */
	  dev->transfer = libusb_alloc_transfer(0);
	  libusb_fill_bulk_transfer(dev->transfer, h, 
			    EP_IMGOUT, dev->buffer, 640*480*3, 
			    image_cb, NULL, TIMEOUT);
	  dev->transfer->flags = LIBUSB_TRANSFER_SHORT_NOT_OK;

	  /* reset state machine */
	  dev->in_transfer = 0;
	  dev->handle = h;
	  devices++;
	}
      }
    }
  }

  /* can only set transfer user_data now as realloc() would have changed it */
  for(i=0;i<devices;i++) {
    (*devs)[i].hdr_transfer->buffer = (void*)&((*devs)[i].hdr);
    (*devs)[i].hdr_transfer->user_data = *devs + i;
    (*devs)[i].transfer->user_data = *devs + i;
  }
  
  libusb_free_device_list(usb_devs, 1);
  return devices;
}

void free_t20(t20_dev_t *devs, int devices) {
  int j;

  for(j=0;j<devices;j++) {
    /* wait for async transfer callback if still in use */
    while(devs[j].in_transfer)
      libusb_handle_events(NULL);

    /* free async transfer itself */
    libusb_free_transfer(devs[j].transfer);

    /* close connection to device */
    libusb_close(devs[j].handle);

    /* and free the transfer buffer if present */
    free(devs[j].buffer);
  }

  /* free buffer and handle lists */
  free(devs);
}

int main(int argc, char *argv[]) {
  int i, j, ret;

  int devices;
  t20_dev_t *devs, *t;

  printf("-- Aiptek T20 demo application --\n");

  if((ret = libusb_init(NULL)) < 0) {
    fprintf(stderr, "USB failed to initialise libusb\n");
    exit(1);
  }
  
  if(!(devices = find_all_t20(&devs))) {
    fprintf(stderr, "Error, could not find any device\n");
    exit(-1);
  }

  if(argc-1 > devices) 
    printf("Warning: %d images given, but only %d devices found\n",
	   argc-1, devices);

  if(devices > argc-1) 
    printf("Warning: %d devices found, but only %d images given\n",
	   devices, argc-1);

  /* upload initial image to all devices */
  for(j=0;j<devices;j++) {

    /* upload image if one was given, otherwise just erase the video */
    write_image(devs+j, (argc < j+2)?NULL:argv[j+1]);
  }

  printf("Running 250 frame performance test ...");
  fflush(stdout);

  /* do some performance testing */
  uint32_t start = get_msec();

  /* this sends async. if there are more than 1 projector, */
  /* both will thus be driven in parallel */
  for(i=0;i<250;i++) 
    for(j=0;j<devices;j++) 
      write_image(devs+j, (argc < j+2)?NULL:argv[j+1]);

  uint32_t total = get_msec() - start;

  printf(" %.2fs\n", total/1000.0);

  /* each single image contains 640*480*3 bytes */
  uint64_t rate = devices*i*640l*480l*3l*1000ll / total;

  printf("Bytes/sec = %" PRIu64 " (%" PRIu64 " mbit/s)\n", 
	 rate, 8*rate>>20);
  printf("  => %f frames/sec\n", rate/devices/(float)(640*480*3));

  /* close connection to all devices and free video buffer */
  free_t20(devs, devices);

  /* and release libusb */
  libusb_exit(NULL);

  return 0;
}
