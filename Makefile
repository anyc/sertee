
APP=sertee

CFLAGS+=$(shell pkg-config fuse3 --cflags) ${USER_CFLAGS}
LDLIBS+=$(shell pkg-config fuse3 --libs) ${USER_LDLIBS}

all: $(APP)

debug: USER_CFLAGS=-DDEBUG
debug: all

clean:
	rm $(APP)
