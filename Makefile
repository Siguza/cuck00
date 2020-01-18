TARGET      = cuck00
SRCDIR      = src
CFLAGS     ?= -O3 -Wall -DPOC=1
SIGN       ?= codesign
SIGN_FLAGS ?= -s -

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCDIR)/*.c $(SRCDIR)/*.h
	$(CC) -o $@ $(SRCDIR)/*.c $(CFLAGS) -framework IOKit -framework CoreFoundation
	$(SIGN) $(SIGN_FLAGS) $@

clean:
	rm -f $(TARGET)
