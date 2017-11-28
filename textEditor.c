/*** includes ***/

#include <ctype.h> 
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h> // Window Size 
#include <termios.h>   // Terminal I/O
#include <unistd.h>

/*** defines ***/


#define TEXTEDITOR_VERSION "0.0.1"

/// bitwise AND with 00011111, equiv to stripping the first 3 bits, what ctrl does
#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  PAGE_UP,
  PAGE_DOWN,
  HOME_KEY,
  END_KEY
};

/*** data ***/
struct settings {
  int cx, cy; //cursor position
  int screenrows;
  int screencols;
  struct termios origTermios;
};

struct settings E;


/*** terminal ***/

void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4); 
  write(STDOUT_FILENO, "\x1b[H", 3);
  perror(s);
  exit(1);
}

void disableRawMode() {
  // checks our library calls for failure, calls die if they fail
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.origTermios) == -1)
    die("tcsetattr");
  
}
void enableRawMode() {
  //save the current console settings. if an error occurs, exit program
  if (tcgetattr(STDIN_FILENO, &E.origTermios) == -1) 
    die("tcgetattr");

  atexit(disableRawMode);                 // disable raw mode when the program exits
  struct termios raw = E.origTermios;      // store the console settings into raw
  
  // disables echoing keypresses
  // enable canonical mode to read input byte-by-byte instead of line-by-line
  // disables ctrl-v
  // disables ctrl-c and ctrl-z signals
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

  // break condition gauses SIGINT to be sent (equiv to pressing ctrl-c)
  // fixes "M" and carriage return commands
  // enables parity checking
  // 8th bit of each byte is stripped
  // disables ctrl-s and ctrl-q flow controls
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

  // disable output processing
  raw.c_oflag &= ~(OPOST);

  //sets character size to 8 bits per byte
  raw.c_cflag |= (CS8);

  raw.c_cc[VMIN] = 0;  // minimum #of bytes to read before read() returns
  raw.c_cc[VTIME] = 1; // max time to wait before read() returns

  // pushes our changes to the terminal attributes. if an error occurs, exit the program
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) 
    die("tcsetattr");

}

// reads in a byte and returns the character, if an error occurs, exit the program
int editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) 
      die("read");
  }



  //if an escape character is read, pull the next 2 characters to see if
  //the user has pressed cntl+chr
  if (c == '\x1b') {
    char seq[3];
    //check to see if the next characters are blank
    if (read(STDIN_FILENO, &seq[0], 1) != 1) 
      return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) 
      return '\x1b';
    
    //we have encountered an escape sequence 
    if (seq[0] == '[') {
          if (seq[1] >= '0' && seq[1] <= '9') {
            if (read(STDIN_FILENO, &seq[2], 1) != 1) 
            	return '\x1b';
            //we have encountered a home/end/page key (multiple possible escape characters for these)
            if (seq[2] == '~') {
              switch (seq[1]) {
              case '1': return HOME_KEY;
              case '3': return DEL_KEY;
              case '4': return END_KEY;
			  case '5': return PAGE_UP;
			  case '6': return PAGE_DOWN;
			  case '7': return HOME_KEY;
			  case '8': return END_KEY;
              }
            }
          } else {
            switch (seq[1]) {
              case 'A': return ARROW_UP;
              case 'B': return ARROW_DOWN;
              case 'C': return ARROW_RIGHT;
              case 'D': return ARROW_LEFT;
              case 'H': return HOME_KEY;
              case 'F': return END_KEY;
            }
          }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
              case 'H': return HOME_KEY;
              case 'F': return END_KEY;
            }
        }
        return '\x1b';
      } else {
        return c;
      }
    }

// on success returns 
int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  //query N command to get cursor position
  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) 
    return -1;

  //read in the characters the N command returned (row and column count)
  // the response is <esc>[24;80R (where 24 and 80 are the row/column count)

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) 
      break;
    if (buf[i] == 'R') 
      break;
    i++;
  }

  // sets the last character of our buffer to a 0 byte (that's what scanf expects)

  buf[i] = '\0';
  // make sure buffer starts with an escape sequence...
  if (buf[0] != '\x1b' || buf[1] != '[') 
    return -1;
  // parse the buffer intwo our row and colum variables
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) 
     return -1;

  return 0;
}

// on success returns 0, sets rows and cols to the size of the console
// on failure returns -1
// should be passed E.screenrows,E.screencols as args
int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

//attempt to set window using ioctl()
if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {

    // easy window dedection did not work, falling back
    // <esc>[999C and <esc>[999B move the cursor to the bottom right corner of the screen,
    // then call getCursorPosition to see how large the window is
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) 
	return -1;
    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}


/*** append buffer ***/
// this is a dynamic string type to update the screen all at once, instead of 
// piecewise with many small write() calls

struct abuf {
  char *b; //pointer to buffer in memory
  int len; // length of the buffer
};

#define ABUF_INIT {NULL, 0} //constructor of our abuf

void abAppend(struct abuf *ab, const char *s, int len) {
  // realloc returns a block of memory big enough to hold the new string
  // it may free() the current block, or return the same block, if enough
  // space already exists to accomidate the new string 's'

  char *new = realloc(ab->b, ab->len + len);
  if (new == NULL) 
    return;

  // copy the new string over, update the pointer and length of abuf
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

// destructor that dellocates dynamic memory used by abuf
void abFree(struct abuf *ab) {
  free(ab->b);
}


/*** output ***/

// draws tildes on rows along the left hand side of the screen.
// the last row does not get a carriage return or new line
void editorDrawRows(struct abuf *ab) {
  int y;
  for (y = 0; y < E.screenrows; y++) {
    
    if(y == E.screenrows / 3){
      char welcome[80];
      int welcomelen = snprintf(welcome, sizeof(welcome),
        "Text Editor -- version %s", TEXTEDITOR_VERSION);
      if (welcomelen > E.screencols) 
        welcomelen = E.screencols;
      int padding = (E.screencols - welcomelen) / 2;
      if (padding) {
        abAppend(ab, "~", 1);
        padding--;
      }
      while (padding--) 
        abAppend(ab, " ", 1);
      abAppend(ab, welcome, welcomelen);
    } else {
      abAppend(ab, "~", 1);
    
    }
    abAppend(ab, "\x1b[K",3); //erases the rest of the line after the tilda
    
    if(y<E.screenrows - 1){
      abAppend(ab, "\r\n", 2);
    }
  }
}

void editorRefreshScreen() {
  struct abuf ab = ABUF_INIT;
  abAppend(&ab, "\x1b[?25l", 6); //disables the cursor while printing to screen

  // writes 3 bytes to the terminal
  // <esc>[H, which sets the cursor to be at row 1, col 1
  // default args for H are <esc>[1;1H
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);
  
  // text below sets the cursor position to our stored value
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
  abAppend(&ab, buf, strlen(buf));


  abAppend(&ab, "\x1b[?25h", 6); //re-enables the cursor after printing to screen

  write(STDOUT_FILENO, ab.b, ab.len); //resets cursor to top of page
  abFree(&ab);
  
}




/*** input ***/

void editorMoveCursor(int key) {
  switch (key) {
    case ARROW_LEFT:
      if(E.cx != 0)
        E.cx--;
      break;
    case ARROW_RIGHT:
      if(E.cx != E.screencols -1)
        E.cx++;
      break;
    case ARROW_UP:
      if(E.cy != 0 )
        E.cy--;
      break;
    case ARROW_DOWN:
      if(E.cy != E.screenrows - 1)
        E.cy++;
      break;
  }
}



//processes input from keypresses
void editorProcessKeypress() {
  int c = editorReadKey();

  switch (c) {
    case CTRL_KEY('q'):
      // if user presses ctrl+q, exit the program
      write(STDOUT_FILENO, "\x1b[2J", 4); 
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;
    case HOME_KEY:
      E.cx = 0;
      break;
    case END_KEY:
      E.cx = E.screencols - 1;
      break;
    case PAGE_UP:
      E.cy = 0;
      break;
    case PAGE_DOWN:
      E.cy = E.screenrows-1;
   
    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editorMoveCursor(c);
      break;
  }
}

/*** init ***/

void initEditor() {
  E.cx = 0;
  E.cy = 0; 
  if (getWindowSize(&E.screenrows, &E.screencols) == -1) 
    die("getWindowSize");
}

int main() {
  enableRawMode();
  initEditor();
while (1) {
    editorRefreshScreen();
    editorProcessKeypress();    
    
  }

  return 0;
}
