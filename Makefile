LIBS=-lgbm -lEGL -lGL -lX11 -lxcb -lX11-xcb -lxcb-dri2 -lxcb-xfixes -ldrm

all:	drm-server drm-client

drm-server:	drm-server.c
	$(CC) -Wall -o $@ $< $(LIBS)

drm-client:	drm-client.c
	$(CC) -Wall -o $@ $< $(LIBS)

.PHONY:	clean

clean:
	rm -f drm-server drm-client

