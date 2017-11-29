// https://viewsourcecode.org/snaptoken/kilo/04.aTextViewer.html#vertical-scrolling
// cursor does not work well with tabs!
/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h> 
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <fcntl.h>    // write and create files
#include <string.h>
#include <sys/ioctl.h> // Window Size 
#include <termios.h>   // Terminal I/O
#include <unistd.h>

/*** defines ***/


#define TEXTEDITOR_VERSION "0.0.1"
#define TAB_STOP 8 // width of our tab stop, default is 8 columns

/// bitwise AND with 00011111, equiv to stripping the first 3 bits, what ctrl does
#define CTRL_KEY(k) ((k) & 0x1f)


enum editorKey {  
  BACKSPACE = 127,
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


/*** prototypes ***/
// function declarations here avoid implicit compile errors

void editorSetStatusMessage(const char *fmt, ...);

/*** data ***/

typedef struct erow {
  int size;      // size of our row
  int rsize;     // render size
  char *chars;   // pointer to our row's data
  char *render;  // 
} erow;

struct settings {
  int cx, cy; 	  //cursor position in the file on row cy, column cx
  int rx;         // index in the render field (used to deal with cursor hopping over tabs)
  int rowoff;     // what row in a file is the user currently on (top of screen)
  int coloff;     // offset for columns, used to display wide files
  int screenrows; // how many rows to display on the screen
  int screencols; // how many cols to display
  int numrows;    // number of rows in the file
  erow *row;	  // pointer to the rows of our files
  char *filename; // the name of the file we are looking at
  char statusmsg[80]; // status message for the menu bar
  time_t statusmsg_time; // time the status message was printed
  struct termios origTermios;
};

struct settings E;


/*** terminal ***/

void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);  // clears screen <esc>[2J
  write(STDOUT_FILENO, "\x1b[H", 3);   // sets cursor to front <esc>[1;1H
  perror(s); 						   // print the error
  exit(1);   						   // exit program
}

void disableRawMode() {
  // resets the terminal config, exits if error occurs
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.origTermios) == -1)
    die("tcsetattr");
  
}
void enableRawMode() {
  //save the current console settings. exits if error occurs
  if (tcgetattr(STDIN_FILENO, &E.origTermios) == -1) 
    die("tcgetattr");
  
  // disable raw mode when the program exits
  atexit(disableRawMode);  
  
  // copies the current console settings into raw
  struct termios raw = E.origTermios;   
  
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

  //if an escape character is read, read the next 2 characters
  if (c == '\x1b') {
    char seq[3];
    //check to see if the next characters are blank, return if so
    if (read(STDIN_FILENO, &seq[0], 1) != 1) 
      return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) 
      return '\x1b';
    
    // check for <esc>[
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
        	// encountered <esc>O
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

// returns 0 on success, -1 on failure to get cursor position
int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  //query N command to get current cursor position <esc>[6N
  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) 
    return -1;

  // N command response is <esc>[24;80R (where 24 and 80 are the row/column count)
  // read in the N commands response from stdin
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
  
  // parse the buffer into our row and colum variables
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



/*** row operations ***/

// determines where to place the cursor, taking into account any tabs
// on the row
int editorRowCxToRx(erow *row, int cx) {
  int rx = 0;
  int j;
  for (j = 0; j < cx; j++) {
    if (row->chars[j] == '\t')
      rx += (TAB_STOP - 1) - (rx % TAB_STOP);
    rx++;
  }
  return rx;
}

// render string is filled with characters from *row
// tabs are replaced with multiple space characters
void editorUpdateRow(erow *row) {
  int tabs = 0;
  int j;
  // count the number of tabs
  for(j = 0;j< row->size;j++)
    if(row->chars[j]=='\t')
    	tabs++;
  
  // allocate enough space for all the characters, plus 8 for each tab (add 7 extras per tab)
  free(row->render);
  row->render = malloc(row->size + tabs*(TAB_STOP-1) + 1);
  
  int idx = 0;
  for (j = 0; j < row->size; j++) {
	if (row->chars[j] == '\t'){
		// append spaces until the next tab stop is reached (every 8 columns by default)
		row->render[idx++] = ' ';
		while(idx % TAB_STOP != 0)
			row->render[idx++] = ' ';
	}else{
    row->render[idx++] = row->chars[j];
    }
  row->render[idx] = '\0';
  row->rsize = idx;
  }
}

// adds a row to our array of rows
void editorAppendRow(char *s, size_t len) {
	// XXXXXXXXXXXXXXXXX
	E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
	
	int at = E.numrows;
	E.row[at].size = len;
	E.row[at].chars = malloc(len + 1);
	memcpy(E.row[at].chars, s, len);
	E.row[at].chars[len] = '\0';
	
	E.row[at].rsize = 0;
	E.row[at].render = NULL;
	editorUpdateRow(&E.row[at]);
	
	E.numrows++;
}

// insert a character into a row
void editorRowInsertChar(erow *row, int at, int c) {
  // make sure the index is valid (allowed to be at the end of the row!)
  if (at < 0 || at > row->size) 
    at = row->size;
  // add two characters to our row (the new character, plus a null byte
  row->chars = realloc(row->chars, row->size + 2);
  // move the characters after the insertion index over one
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  editorUpdateRow(row);
}

/*** editor operations ***/

// adds a chacter to the file at the current cursor location
void editorInsertChar(int c) {
  // if the cursor is at the bottom, add a new row
  if (E.cy == E.numrows) {
    editorAppendRow("", 0);
  }
  // insert the character at the position, then increment the cursor one
  editorRowInsertChar(&E.row[E.cy], E.cx, c);
  E.cx++;
}


/*** file i/o ***/

// converts our array of rows into a string for writing to a file
char *convertRowsToString(int *buflen) {
  int totlen = 0;
  int j;
  // add up the lenghts of each row, plus 1 for the newline char on each row
  for (j = 0; j < E.numrows; j++)
    totlen += E.row[j].size + 1;
  *buflen = totlen;
  char *buf = malloc(totlen);
  char *p = buf;
  for (j = 0; j < E.numrows; j++) {
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    *p = '\n';
    p++;
  }
  return buf;
}

// opens a file, passed as the first arg when running the program
void editorOpen(char *filename) {
  free(E.filename);
  E.filename = strdup(filename);
  
  FILE *fp = fopen(filename, "r");
  if (!fp) 
    die("fopen");
  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
      while (linelen > 0 && (line[linelen - 1] == '\n' ||
                             line[linelen - 1] == '\r'))
        linelen--;
      editorAppendRow(line, linelen);
    }
    free(line);
    fclose(fp);
  }

// saves the file
void editorSave() {
  if (E.filename == NULL) 
    return;
  int len;
  // get a string to write to the new file
  char *buf = convertRowsToString(&len);
  // open the file, or create a new file with the correct file name
  // the owner gets r/w access, other people only get read access with permission code 0644
  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
  if (fd != -1) {
	  // shortens the file size to exactly the required length
	  if (ftruncate(fd, len) != -1) {
        if (write(fd, buf, len) == len) {
        	close(fd);
        	free(buf);
            editorSetStatusMessage("%d bytes written to disk", len);
        	return;
        }
	  }
	  close(fd); // error occured, close file
  }
  free(buf); // error occured, free memory
  editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
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

// if cursor is moved off screen, modify row offset to scroll up/down
// and modify col offset to scroll left/right 
// boundries check to make sure you don't scroll off screen!
void editorScroll() {
  E.rx = 0;
  if (E.cy < E.numrows) {
      E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
  }
  
  if (E.cy < E.rowoff) {
    E.rowoff = E.cy;
  }
  if (E.cy >= E.rowoff + E.screenrows) {
    E.rowoff = E.cy - E.screenrows + 1;
  }
  if (E.rx < E.coloff) {
      E.coloff = E.rx;
  }
  if (E.rx >= E.coloff + E.screencols) {
      E.coloff = E.rx - E.screencols + 1;
  }
}

// draws tildes on rows along the left hand side of the screen.
// the last row does not get a carriage return or new line
// starts writing rows at the top of the screen
// beginning at the row index filerow
void editorDrawRows(struct abuf *ab) {
  int y;
  // loop through all the availible terminal rows, print out our lines
  for (y = 0; y < E.screenrows; y++) {
	  int filerow = y+E.rowoff;

  	// check to see if a row exists in our file
    if(filerow>=E.numrows){
    	// print a welcome message 1/3 down the screen if the file is empty 
        if (E.numrows == 0 && y == E.screenrows / 3) {
          char welcome[80];
          int welcomelen = snprintf(welcome, sizeof(welcome),
          "Text Editor -- version %s", TEXTEDITOR_VERSION);
          // truncate welcome message if too long
          if (welcomelen > E.screencols) 
            welcomelen = E.screencols;
          int padding = (E.screencols - welcomelen) / 2;
          
          if (padding) {
            abAppend(ab, "~", 1);
          padding--;
          }
          // center welcome message on the screen
          while (padding--) 
            abAppend(ab, " ", 1);
          abAppend(ab, welcome, welcomelen);
          
    } else {
      // print a tilde on the front of any blank line for null rows at the end of our file
      abAppend(ab, "~", 1);
    
    } 
    } else {
    	// get the length of the current row of the file, minus any column offset
        int len = E.row[filerow].rsize - E.coloff;
        if (len < 0 )
        	len = 0;
        
        // truncate the row if it is too wide to display
        if (len > E.screencols) 
        	  len = E.screencols;
        // add the row to ab
        abAppend(ab, &E.row[filerow].render[E.coloff], len);
      }
    // erases the rest of the line after the tilda
    abAppend(ab, "\x1b[K",3); 
    
    // add an empty line at the very end of the terminal window, which will
    // become the status bar
    abAppend(ab, "\r\n", 2);
    }
  
}

void editorDrawStatusBar(struct abuf *ab) {
  // <esc>[7M inverts the colors for our status bar
  abAppend(ab, "\x1b[7m", 4);
  char status[80], rstatus[80];
  // prints the file name and number of lines
  int len = snprintf(status, sizeof(status), "%.20s - %d lines",
    E.filename ? E.filename : "[No File Opened]", E.numrows);

  // print the current line on the right side of the screen
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
    E.cy + 1, E.numrows);
  
  if (len > E.screencols) 
	len = E.screencols;
  abAppend(ab, status, len);
  // draw our status line
  while (len < E.screencols) {
    if (E.screencols - len == rlen) {
      abAppend(ab, rstatus, rlen);
	  break;
	} else {
      abAppend(ab, " ", 1);
      len++;
    }
  }
  // <esc>[M returns our color scheme to normal
  abAppend(ab, "\x1b[m", 3);
  // add a row below our status bar to display any status messages
  abAppend(ab, "\r\n",2);
}

void editorDrawMessageBar(struct abuf *ab) {
  // clear the row
  abAppend(ab, "\x1b[K", 3);
  // add our message
  int msglen = strlen(E.statusmsg);
  if (msglen > E.screencols) msglen = E.screencols;
  // only draw the message if it is less than 5 sec old!
  if (msglen && time(NULL) - E.statusmsg_time < 5)
    abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen() {
  // update the current scroll position
  editorScroll();
  
  // ab is the chacters to be displayed on our screen
  struct abuf ab = ABUF_INIT;
  abAppend(&ab, "\x1b[?25l", 6); //disables the cursor while printing to screen

  // writes 3 bytes to the terminal
  // <esc>[H, which sets the cursor to be at row 1, col 1
  // default args for H are <esc>[1;1H
  abAppend(&ab, "\x1b[H", 3);

  // add enough rows to fill the screen to ab
  editorDrawRows(&ab);
  
  // add the status bar and message bars to the end of ab
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);
  
  // sets the cursor position on the screen to our stored value (cx,cy)
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
  abAppend(&ab, buf, strlen(buf));

  // re-enables the cursor after printing to screen
  abAppend(&ab, "\x1b[?25h", 6); 

  // writes all the rows to our screen at once
  write(STDOUT_FILENO, ab.b, ab.len); 
  
  // free memory space
  abFree(&ab);
  
}

// sets the status message on the menu bar
void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}


/*** input ***/

// moves the cursor using the arrow keys, being careful not to go out of bounds
// scrolls if possible up and down the file
void editorMoveCursor(int key) {
  // fetch current row, so that you cannot scroll too far to the right
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  
  switch (key) {
    // moves cursor left, unless cursor is on left edge of row, 
    // then moves the cursor up a line to the end of the previous row
    case ARROW_LEFT:
	  if (E.cx != 0) {
	    E.cx--;
	  } else if (E.cy > 0) {
		E.cy--;
		E.cx = E.row[E.cy].size;
	  }
	  break;
	// moves cursor right, unless cursor is on right edge of row,
	// then moves the cursor down a line to the start of the next row
    case ARROW_RIGHT:
      if (row && E.cx < row->size) {
        E.cx++;
      } else if (row && E.cx == row->size) {
        E.cy++;
        E.cx = 0;
      }
      break;
    case ARROW_UP:
      if(E.cy != 0 )
        E.cy--;
      break;
    case ARROW_DOWN:
      if(E.cy < E.numrows )
        E.cy++;
      break;
  }
  
  // snap cursor to end of row if user switched from a long row to a shorter row
  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  int rowlen = row ? row->size : 0;
  if (E.cx > rowlen)
    E.cx = rowlen;
}



//processes input from keypresses
void editorProcessKeypress() {
	
  // reads in a key
  int c = editorReadKey();

  switch (c) {
    case '\r':
	  break;
	  
    case CTRL_KEY('q'):
      // if user presses ctrl+q, exit the program
      write(STDOUT_FILENO, "\x1b[2J", 4); // clears the screen <esc>[2J
      write(STDOUT_FILENO, "\x1b[H", 3);  // sets cursor to top left <esc>[1;1H
      exit(0);							  // exits program without error
      break;
      
    case CTRL_KEY('s'):
      editorSave();
      break;
          
    case HOME_KEY:
      E.cx = 0;
      break;
    case END_KEY:
      if (E.cy < E.numrows)
        E.cx = E.row[E.cy].size;
      break;
    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
      /* TODO */
      break;
    // page up/down positions cursor at top/bottom of screen, then calls
    // one screen's worth of up/down arrow presses
    case PAGE_UP:
    case PAGE_DOWN:
    {
      if (c == PAGE_UP) {
        E.cy = E.rowoff;
      } else if (c == PAGE_DOWN) {
        E.cy = E.rowoff + E.screenrows - 1;
        if (E.cy > E.numrows) 
          E.cy = E.numrows;
        }
      int times = E.screenrows;
      while (times--)
        editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
     }
     break;
   
    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editorMoveCursor(c);
      break;
    // disables the refresh <ctrl>+l keypair and any excape sequences
    case CTRL_KEY('l'):
    case '\x1b':
      break;
    default:
      editorInsertChar(c);
      break;
  }
}

/*** init ***/

void initEditor() {
  
  // sets cursor to top left of screen
  E.cx = 0;
  E.cy = 0; 
  E.rx = 0;
  
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0;
  E.row = NULL;
  E.filename = NULL;
  
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;
  
  // determines how many rows/cols the terminal can display
  if (getWindowSize(&E.screenrows, &E.screencols) == -1) 
    die("getWindowSize");
  
  // the second-to-last row of our terminal is free to display a status bar
  // the last row will display any messages to the user
  E.screenrows -= 2;
}

int main( int argc, char *argv[] ) {
  enableRawMode();
  initEditor();

  // if a filename was passed as an arg, open the file
  if( argc >= 2){
    editorOpen(argv[1]);
  }
  
  // initial status message
  editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit");

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();    
    
  }

  return 0;
}
