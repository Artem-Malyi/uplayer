
PROG_NAME=uplayer
SOURCE_FILES= ${PROG_NAME}.c
CFLAGS+=-I. -Wno-deprecated-declarations -Wno-format-security
LDFLAGS+=-L. -lavformat

all:
	$(CC) -o ${PROG_NAME} ${CFLAGS} ${LDFLAGS} ${SOURCE_FILES}

clean:
	$(RM) ${PROG_NAME}