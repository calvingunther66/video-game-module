# Host build of the desktop renderer; writes PNG previews to out/.
APP := ../../app/desktop
SRC := preview.c $(APP)/gfx.c $(APP)/desktop.c $(APP)/assets_gen.c
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -std=c11 -fsanitize=address,undefined

all: out
	./preview frames/desktop.bin out

preview: $(SRC) $(wildcard $(APP)/*.h)
	$(CC) $(CFLAGS) -I$(APP) -o $@ $(SRC)

out: preview
	mkdir -p out

clean:
	rm -rf preview preview.dSYM out

.PHONY: all clean
