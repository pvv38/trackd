.SUFFIXES: .c .h .o .a

# Target.
TARG =		trackd

# Chose compiler You prefere.
CC =		gcc
CFLAGS=		-c -O2 

DEFINES =
INCLUDES =

# Chose linker You prefere.
LD =		gcc
LFLAGS =	-L/usr/local/pgsql/lib -lpq -lcrypt

# File lists.
INCS =		
SRCS =		$(TARG).c db.c
OBJS =		$(SRCS:.c=.o)

# Default way in which source files should be compiled.
.c.o : 
		$(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) -o $@ $<

# Custom targets.
$(TARG) : $(OBJS)
	($(LD) $(LFLAGS) -o $@ $(OBJS))
