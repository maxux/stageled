EXEC = stage-control
SRC = $(wildcard *.c)
OBJ = $(SRC:.c=.o)

CFLAGS += -g -W -Wall -O2
LDFLAGS += -lpng -lasound -lpthread

all: $(EXEC)

$(EXEC): $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $<

clean:
	$(RM) *.o

mrproper: clean
	$(RM) $(EXEC)
