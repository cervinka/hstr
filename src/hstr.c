/*
 ============================================================================
 Name        : hstr.c
 Author      : Martin Dvorak
 Version     : 0.2
 Copyright   : Apache 2.0
 Description : Shell history completion utility
 ============================================================================
*/

#include <curses.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include "include/hashset.h"

#define HSTR_VERSION "0.2"

#define LABEL_HISTORY " HISTORY "
#define LABEL_HELP "Type to filter history, use UP and DOWN arrows to navigate, ENTER to select"
#define ENV_VAR_USER "USER"
#define ENV_VAR_HOME "HOME"
#define FILE_HISTORY ".bash_history"
#define SELECTION_CURSOR_IN_PROMPT -1

#define Y_OFFSET_PROMPT 0
#define Y_OFFSET_HELP 2
#define Y_OFFSET_HISTORY 3
#define Y_OFFSET_ITEMS 4

#define KEY_TERMINAL_RESIZE 410
#define KEY_CTRL_A 1
#define KEY_CTRL_E 5

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

#ifdef DEBUG_KEYS
#define LOGKEYS(Y,KEY) mvprintw(Y, 0, "Key number: '%3d' / Char: '%c'", KEY, KEY)
#else
#define LOGKEYS(Y,KEY) ;
#endif

#ifdef DEBUG_CURPOS
#define LOGCURSOR(Y) mvprintw(Y, 0, "X/Y: %3d / %3d", getcurx(stdscr), getcury(stdscr))
#else
#define LOGCURSOR(Y) ;
#endif

static char ** selection=NULL;
static int selectionSize=0;
static bool terminalHasColors=FALSE;


int printPrompt(WINDOW *win) {
	char hostname[128];
	char *user = getenv(ENV_VAR_USER);
	int xoffset = 1;

	gethostname(hostname, sizeof hostname);
	mvwprintw(win, xoffset, Y_OFFSET_PROMPT, "%s@%s$ ", user, hostname);
	refresh();

	return xoffset+strlen(user)+1+strlen(hostname)+1;
}

void printHelpLabel(WINDOW *win) {
	mvwprintw(win, Y_OFFSET_HELP, 0, LABEL_HELP);
	refresh();
}

void printHistoryLabel(WINDOW *win) {
	char message[512];

	int width=getmaxx(win);

	strcpy(message, LABEL_HISTORY);
	width -= strlen(LABEL_HISTORY);
	int i;
	for (i=0; i < width; i++) {
		strcat(message, " ");
	}

	wattron(win, A_REVERSE);
	mvwprintw(win, Y_OFFSET_HISTORY, 0, message);
	wattroff(win, A_REVERSE);

	refresh();
}

int getMaxHistoryItems(WINDOW *win) {
	return (getmaxy(win)-(Y_OFFSET_ITEMS+2));
}

char *loadHistoryFile() {
	char *home = getenv(ENV_VAR_HOME);
	char *fileName=(char*)malloc(strlen(home)+1+strlen(FILE_HISTORY)+1);
	strcpy(fileName,home);
	strcat(fileName,"/");
	strcat(fileName,FILE_HISTORY);

	if(access(fileName, F_OK) != -1) {
		char *file_contents;
		long input_file_size;

		FILE *input_file = fopen(fileName, "rb");
		fseek(input_file, 0, SEEK_END);
		input_file_size = ftell(input_file);
		rewind(input_file);
		file_contents = malloc((input_file_size + 1) * (sizeof(char)));
		if(fread(file_contents, sizeof(char), input_file_size, input_file)==-1) {
			exit(EXIT_FAILURE);
		}
		fclose(input_file);
		file_contents[input_file_size] = 0;

		return file_contents;
	} else {
	    fprintf(stderr,"\nHistory file not found: %s\n",fileName);
	    exit(EXIT_FAILURE);
	}
}

int countHistoryLines(char *history) {
	int i = 0;
	char *p=strchr(history,'\n');
	while (p!=NULL) {
		i++;
		p=strchr(p+1,'\n');
	}
	return i;
}

char **tokenizeHistory(char *history, int lines) {
	char **tokens = malloc(sizeof(char*) * lines);

	int i = 0;
	char *pb=history, *pe;
	pe=strchr(history, '\n');
	while(pe!=NULL) {
		tokens[i]=pb;
		*pe=0;

		pb=pe+1;
		pe=strchr(pb, '\n');
		i++;
	}

	return tokens;
}

void allocSelection(int size) {
	selectionSize=size;
	if(selection!=NULL) {
		free(selection);
		selection=NULL;
	}
	if(size>0) {
		selection = malloc(size);
	}
}

int makeSelection(char* prefix, char **historyFileItems, int historyFileItemsCount, int maxSelectionCount) {
	allocSelection(sizeof(char*) * maxSelectionCount); // TODO realloc
	int i, selectionCount=0;

    HashSet set;
    hs_init(&set);

	for(i=0; i<historyFileItemsCount && selectionCount<maxSelectionCount; i++) {
		if(!hs_contains(&set, historyFileItems[i])) {
			if(prefix==NULL) {
				selection[selectionCount++]=historyFileItems[i];
				hs_add(&set, historyFileItems[i]);
			} else {
				if(historyFileItems[i]==strstr(historyFileItems[i], prefix)) {
					selection[selectionCount++]=historyFileItems[i];
					hs_add(&set, historyFileItems[i]);
				}
			}
		}
	}

	if(prefix!=NULL && selectionCount<maxSelectionCount) {
		for(i=0; i<historyFileItemsCount && selectionCount<maxSelectionCount; i++) {
			if(!hs_contains(&set, historyFileItems[i])) {
				char* substring = strstr(historyFileItems[i], prefix);
				if (substring != NULL && substring!=historyFileItems[i]) {
					selection[selectionCount++]=historyFileItems[i];
					hs_add(&set, historyFileItems[i]);
				}
			}
		}
	}

	selectionSize=selectionCount;
	return selectionCount;
}

char* printSelection(WINDOW *win, int maxHistoryItems, char *prefix, int historyFileItemsCount, char** historyFileItems) {
	char* result="";
	int selectionCount=makeSelection(prefix, historyFileItems, historyFileItemsCount, maxHistoryItems);
	if (selectionCount > 0) {
		result = selection[0];
	}

	int height=getMaxHistoryItems(win);
	int i;
	int y=Y_OFFSET_ITEMS;
	for (i = 0; i<height; ++i) {
		if(i<selectionSize) {
			mvwprintw(win, y++, 1, "%s", selection[i]);
			clrtoeol();
			if(prefix!=NULL) {
				wattron(win,A_BOLD);
				char *p=strstr(selection[i], prefix);
				mvwprintw(win, (y-1), 1+(p-selection[i]), "%s", prefix);
				wattroff(win,A_BOLD);
			}
		} else {
			mvwprintw(win, y++, 0, " ");
			clrtoeol();
		}
	}
	refresh();

	return result;
}

void highlightSelection(int selectionCursorPosition, int previousSelectionCursorPosition) {
	if(previousSelectionCursorPosition!=SELECTION_CURSOR_IN_PROMPT) {
		mvprintw(Y_OFFSET_ITEMS+previousSelectionCursorPosition, 0, " ");
	}
	if(selectionCursorPosition!=SELECTION_CURSOR_IN_PROMPT) {
		mvprintw(Y_OFFSET_ITEMS+selectionCursorPosition, 0, ">");
	}
}

void colorStart() {
	terminalHasColors=has_colors();
	if(terminalHasColors) {
		start_color();
	}
}

void colorInitPair(short int x, short int y, short int z) {
	if(terminalHasColors) {
		init_pair(x, y, z);
	}
}

void colorAttron(int c) {
	if(terminalHasColors) {
		attron(c);
	}
}

void colorAttroff(int c) {
	if(terminalHasColors) {
		attroff(c);
	}
}

char* selectionLoop(char **historyFileItems, int historyFileItemsCount) {
	initscr();
	colorStart();

	colorInitPair(1, COLOR_WHITE, COLOR_BLACK);
	colorAttron(COLOR_PAIR(1));
	printHistoryLabel(stdscr);
	printHelpLabel(stdscr);
	printSelection(stdscr, getMaxHistoryItems(stdscr), NULL, historyFileItemsCount, historyFileItems);
	int basex = printPrompt(stdscr);
	int x = basex;
	colorAttroff(COLOR_PAIR(1));

	int selectionCursorPosition=SELECTION_CURSOR_IN_PROMPT;
	int previousSelectionCursorPosition=SELECTION_CURSOR_IN_PROMPT;

	int y = 1, c, maxHistoryItems, cursorX, cursorY;
	bool done = FALSE;
	char prefix[500]="";
	char* result="";
	while (!done) {
		maxHistoryItems=getMaxHistoryItems(stdscr);

		noecho();
		c = wgetch(stdscr);
		echo();

		switch (c) {
		case KEY_TERMINAL_RESIZE:
		case KEY_CTRL_A:
		case KEY_CTRL_E:
			break;
		case 91:
			// TODO 91 killed > debug to determine how to distinguish \e and [
	        //mvprintw(Y_OFFSET_HELP, 0, "91 killed");
			break;
		case KEY_BACKSPACE:
		case 127:
			if(strlen(prefix)>0) {
				prefix[strlen(prefix)-1]=0;
				x--;
				wattron(stdscr,A_BOLD);
				mvprintw(y, basex, "%s", prefix);
				wattroff(stdscr,A_BOLD);
				clrtoeol();
			}

			if(strlen(prefix)>0) {
				makeSelection(prefix, historyFileItems, historyFileItemsCount, maxHistoryItems);
			} else {
				makeSelection(NULL, historyFileItems, historyFileItemsCount, maxHistoryItems);
			}
			result = printSelection(stdscr, maxHistoryItems, prefix, historyFileItemsCount, historyFileItems);
			break;
		case KEY_UP:
		case 65:
			if(selectionCursorPosition>SELECTION_CURSOR_IN_PROMPT) {
				previousSelectionCursorPosition=selectionCursorPosition;
				selectionCursorPosition--;
			} else {
				previousSelectionCursorPosition=SELECTION_CURSOR_IN_PROMPT;
			}
			highlightSelection(selectionCursorPosition, previousSelectionCursorPosition);
			break;
		case KEY_DOWN:
		case 66:
			previousSelectionCursorPosition=selectionCursorPosition;
			if((selectionCursorPosition+1)<selectionSize) {
				selectionCursorPosition++;
			} else {
				selectionCursorPosition=0;
			}
			highlightSelection(selectionCursorPosition, previousSelectionCursorPosition);
			break;
		case 10:
			if(selectionCursorPosition!=SELECTION_CURSOR_IN_PROMPT) {
				result=selection[selectionCursorPosition];
				allocSelection(0);
			}
			done = TRUE;
			break;
		default:
			LOGKEYS(Y_OFFSET_HELP,c);
			LOGCURSOR(Y_OFFSET_HELP);

			if(c!=27) {
				strcat(prefix, (char*)(&c));
				wattron(stdscr,A_BOLD);
				mvprintw(y, basex, "%s", prefix);
				cursorX=getcurx(stdscr);
				cursorY=getcury(stdscr);
				wattroff(stdscr,A_BOLD);
				clrtoeol();

				result = printSelection(stdscr, maxHistoryItems, prefix, historyFileItemsCount, historyFileItems);
				//wmove(stdscr, cursorX, cursorY);
				refresh();
			}
			break;
		}
	}
	endwin();

	return result;
}

void tiocsti() {
	char buf[] = "cmd";
	int i;
	for (i = 0; i < sizeof buf - 1; i++) {
		ioctl(0, TIOCSTI, &buf[i]);
	}
}

void fillTerminalInput(char* cmd){
	size_t size = strlen(cmd);
	int i;
	char *c;
	for (i = 0; i < size; i++) {
		// terminal I/O control, simulate terminal input
		c=(cmd+i);
		ioctl(0, TIOCSTI, c);
	}
	printf("\n");
}

void reverseCharPointerArray(char **array, int length) {
	int i;
	char * temp;
    for (i=0; i<length/2; i++) {
        temp = array[i];
        array[i] = array[length-i-1];
        array[length-i-1] = temp;
    }
}

void hstr() {
	char *historyAsString = loadHistoryFile(FILE_HISTORY);
	int itemsCount = countHistoryLines(historyAsString);
	char** items = tokenizeHistory(historyAsString, itemsCount);
	reverseCharPointerArray(items, itemsCount);
	char* command = selectionLoop(items, itemsCount);
	fillTerminalInput(command);
	free(historyAsString);
	free(items);
}

int main(int argc, char *argv[]) {
	hstr();
	return EXIT_SUCCESS;
}

