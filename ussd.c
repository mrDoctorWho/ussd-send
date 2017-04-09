#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <iconv.h>
#include <errno.h>
#include <stdbool.h>
#include <getopt.h>
#include <stdarg.h>


#define AT_CUSD "AT+CUSD=1"
#define MODEM_TTY "/dev/ttyUSB1"
bool DEBUG = false;

void color(char *line, char result[], int color) {
	sprintf(result, "%c[1;%dm%s%c[0m", 27, color, line, 27);  // WE NEED MORE FORMATTING!
}

void debug(char *format, ...) {
	if (DEBUG) {
		char text[strlen(format)+15]; // add 15 for formatting stuff
		color(format, text, 33);
		va_list args;
		va_start(args, format);
		vprintf(text, args);
		va_end(args);
	}
}

/* Encoders */
char *to7bit(char *line) {
	/* Encodes a usual char array into a 7-bit char array */
	int count = 0;
	int last = 0;
	char *result = (char*)malloc(strlen(line) * 2 + 1);
	for (int c = 0; c <= strlen(line); c++) {
		int current = (line[c]) << (8-count);
		if (count) {
			sprintf(result, "%s%02X", result, ((last>>8) | (current & 0xFF)));
		}
		count = (count + 1) % 8;
		last = current;
	}
	/* No idea if the following line is required.
	   Uncomment if you run in any decode problems. */
	//sprintf(result, "%s%02x", result, last>>8);
	result[strlen(result)+1] = 0;
	debug("7bit encode result: %s\n", result);
	return result;
}

/* Decoders */
int hextostr(char *input, char result[]) {
	/* Performs hex decoding (base16) */
	int in_length = strlen(input);
	if (in_length & 1) {
		printf("Odd length for input line!\n");
		return 0;
	}
	int i=0;
	int out_length=0;
	for (int i=0; i<in_length; i+=2) {
		char temp[] = {input[i], input[i+1]};
		char c = strtol(temp, NULL, 16);
		result[out_length] = c;
		out_length++;
	}
	result[out_length+1]=0;
	return out_length;
}


int utf16to8(iconv_t conv, char utf16[], char utf8[], size_t utf16len, size_t utf8len)
{
	/* Performs utf16 to utf8 encoding
	   Unfortunately, utf8 takes up to 4 bytes, while utf16 can be either 2 or 4.
	   I have no idea how to guess the result length.
	*/
	size_t iconv_value = iconv(conv, &utf16, &utf16len, &utf8, &utf8len);
	if (iconv_value == (size_t) -1) {
		switch (errno) {
			case EILSEQ:
				fprintf (stderr, "iconv: invalid multibyte sequence.\n");
				break;
			case EINVAL:
				fprintf (stderr, "iconv: incomplete multibyte sequence.\n");
				break;
			case E2BIG:
				fprintf (stderr, "iconv: no more room.\n");
				break;
			default:
				fprintf (stderr, "iconv: error: %s.\n", strerror (errno));
		}
		return -1;
	}
	return utf8len;
}


void format(char *cmd, char *args, char result[]) {
	sprintf(result, "%s,\"%s\",15\r\n", cmd, args);
/*
	// old stuff
	strcat(result, cmd);
	strcat(result, ",\"");
	strcat(result, args);
	strcat(result, "\",15\r\n"); // TODO: We don't need "15" at the end if it's not the CUSD command
*/
}


int send(int fd, char *command) {
	printf("Sending command: %s\n", command);
	strcat(command, "\r\n");
	return write(fd, command, strlen(command));
}


void help(char *const *argv) {
	fprintf(stderr, "Usage: %s [-acdtu]\n"\
					"Example: %s -u *100# -t /dev/ttyUSB0\n"\
					"Note: program does USSD response decode through decoding hex and then decoding hex from UTF16BE using the GNU Iconv library\n\n"\
					"\t-a <args>\t\tcommand arguments (not enables 7-bit encoding), conflicts with -u\n"\
					"\t-c <command>\t\tcommand you want the modem to call, conflicts with -u\n"\
					"\t-d\t\t\tenable debug\n"
					"\t-t <TTY>\t\tset tty other than /dev/ttyUSB1\n"\
					"\t-u <USSD Request>\tmostly same to -a, but performs 7-bit encoding (required if no -a set)\n", argv[0], argv[0]);
	exit(EXIT_FAILURE);
}


void findPart(char *line, char result[]) {
	/* Finds the string which follows "AT+" */
	char *str = line;
	char *res = result;
	bool started = false;
	while (*str++) {
		if (!started) {
			if (*str == '+') {
				started = true;
				*result = '+';
				result++;
			}
		}
		else {
			if (*str == '=' || *str == ',' || *str == ':') {
				break;
			}
			*result = *str;
			result++;
		}
	}
	*result = ':';
	result++;
	*result = 0;
}


void findQuotedString(char *line, char result[]) {
	/* Finds string inside the double quotes */
	// WARNING: If the line has no quotes program will FAIL
	char *str = line;
	char *res = result;
	bool started = false;
	char *c;
	while (*str++) {
		if (!started) {
			if (*str == '"') {
				started = true;
			}
		}
		else {
			if (*str == '"') {
				break;
			}
			*result = *str;
			result++;
		}
	}
	*result = 0;
}


int main(int argc, char *const *argv) {
	char *tty, *cmd, *args, *ussd;
	tty = cmd = args = ussd = NULL;
	int fd, opt;
	bool decode, encode;
	if (argc < 2) {
		help(argv);
	}
	while ((opt = getopt(argc, argv, "a:c:dt:u:")) != -1) {
		switch (opt) {
			case 't':
				tty = optarg;
				break;
			case 'c':
				printf("cmd: %s\n",optarg);
				cmd = optarg;
				break;
			case 'u':
				ussd = optarg;
				break;
			case 'd':
				DEBUG = true;
				break;
			case 'a':
				printf("args: %s\n",optarg);
				args = optarg;
				break;
			default:
				help(argv);
			}
	}
	/* TODO: check if SUID is set by checking if main.bin has u+s rights */
	if (geteuid() != 0) {
		printf("Sorry, root priveleges required\n");
		exit(EXIT_FAILURE);
	}
	/* Show help if no one of the requried parameters was set. */
	if (!cmd && !args && !ussd) {
		fprintf(stderr, "Error: missing parameters.\n");
		help(argv);
	}
	if ((cmd && !args || args && !cmd) && !ussd) {
		help(argv);
	}

	if (tty == NULL) {
		debug("Warning: No tty set, using %s.\n", MODEM_TTY);
		tty = MODEM_TTY;
	}
	/* If user tries to set cmd plus ussd which are in conflict. */
	if (cmd && ussd) {
		fprintf(stderr, "Error: conflicting arguments. You can use either -u or -c, not both.\n");
		exit(EXIT_FAILURE);
	}
	/* If user tries to set args plus ussd which are in conflict. */
	if (args && ussd) {
		fprintf(stderr, "Error: conflicting arguments. You can use either -u -or -a, not both.\n");
		exit(EXIT_FAILURE);
	}
	/* If no command set. */
	if (!cmd) {
		printf("No command set, using %s.\n", AT_CUSD);
		cmd = AT_CUSD;
	}
	/* If no args but ussd set, prefer ussd to args */
	if (!args && ussd) {
		args = to7bit(ussd);
	}
	if (!args) {
		fprintf(stderr, "Please specify -a or -u option.\n");
		exit(EXIT_FAILURE);
	}

	fd = open(tty, O_RDWR);

	if (!fd) {
		printf("Unable to open %s, exitting.\n", tty);
		exit(EXIT_FAILURE);
	}
	/* command + args + fomatting length */
	int length = strlen(cmd) + (strlen(args) * 2);
	debug("Command + args length: %d\n",length);

	/* part of our command that we'll use to find the answer & the command itslef*/
//	char expected[length], command[length];
	char *expected, *command;
	expected = (char*)malloc(sizeof(char) * length);
	command = (char*)malloc(sizeof(char) * length);

	/* searching for our expected part */
	findPart(cmd, expected);

	/* formatting */
	format(cmd, args, command);
	
	/* the string we'll search in the modem answers */
	debug("Expecting: '%s'\n", expected);
	debug("Command: '%s'\n", command);

	/* TODO: prevent overflow */
	char *last_answer = (char*)malloc(sizeof(char)*1000);
	char *result = (char*)malloc(sizeof(char)*1000);
	if (send(fd, command)) {
		printf("Command sent, waiting for answer.\n");
		while (true) {
			int nbytes = 0;
			while (true) {
				char temp[1];
				int i = read(fd, temp, 1);
				if (i == -1) {
					fprintf(stderr, "error: %s", strerror(errno));
					exit(EXIT_FAILURE);
				}
				/* Line ends by \n and we're done with it */
				if (*temp == '\n') {
					break;
				}
				/* TODO: Perform a check if we have free space. What if we don't? */
				result[nbytes] = temp[0];
				nbytes++;
			}
			result[nbytes] = 0;
			debug("Read %d bytes\n", nbytes);
			/* Preventing read of the same answer twice. Does it work btw? */
			if (last_answer && !strcmp(result, last_answer)) {
				continue;
			}
			strcpy(last_answer, result);
			if (!strcmp(result, command)) {
				continue;
			}
			/* When our modem finally answers. We search for answer starting from "+CUSD:"" */
			if (!strncmp(result, expected, strlen(expected))) {
				debug("Modem answers: %s\n", result);
				char answer[strlen(result)];
				findQuotedString(result, answer);
				debug("Unqoted answer: %s\n", answer);

				/* Decoding the answer */
				int utf8_length, utf16_length;
				/* We've got hex as the answer, so it'll be half-sized after decode */
				utf8_length = strlen(answer)/2;
				/* Though utf16 length is unclear */
				char utf16[strlen(answer) + 1];
				utf16_length = hextostr(answer, utf16);
				iconv_t conv = iconv_open("UTF-8", "UTF-16BE");
				char *utf8 = calloc(utf8_length, 1);
				utf16to8 (conv, utf16, utf8, utf16_length, utf8_length);
				printf("Result: %s\n", utf8);
				free(utf8);
				break;

			}
			debug("Modem says: %s\n", result);
			debug("-------\n");
			sleep(1);
		}
	}
	free(last_answer);
	free(result);
	if (ussd) {
		free(args);
	}
	close(fd);
}