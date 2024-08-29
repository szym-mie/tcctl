CC = clang
CCF += -s
CCF += -Wall
LIB += -lcap

TARGET=tcctl

$(TARGET): %: %.c
	$(CC) $(CCF) -o $@ $^ $(LIB)

.PHONY: install
install:
	cp $(TARGET) /bin
	setcap cap_sys_rawio+ep $(TARGET)
	setcap cap_sys_rawio+ep /bin/$(TARGET)

.PHONY: clean
clean:
	rm -f $(TARGET)
