/**
        wdired - a standalone emacs-like text-based directory editor
        author: David Yue <david.yue@utah.edu>
*/

#include <assert.h>
#include <dirent.h>
#include <getopt.h>
#include <grp.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dirent.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <sys/wait.h>
#include <unistd.h>

#define ALPHAS         "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
#define NUMERIC        "0123456789"
#define SPECIAL        "-_.,;=+!@#$%%^&()[]{}'`~"
#define FILESAFE_CHARS ALPHAS NUMERIC "-_.,;=+!@#$%%^&()[]{}'`~"
#define SEQUENCE(name, ...)                                                                                            \
        static bool name (struct Parser *p, struct ParseResult *r)                                                     \
        {                                                                                                              \
                ParseFn args[]      = {__VA_ARGS__};                                                                   \
                struct ParseState s = saveParseState (p, r);                                                           \
                for (int i = 0; i < sizeof (args) / sizeof (*args); i++) {                                             \
                        if (!args[i](p, r)) {                                                                          \
                                restoreParseState (p, r, s);                                                           \
                                return false;                                                                          \
                        }                                                                                              \
                }                                                                                                      \
                                                                                                                       \
                return true;                                                                                           \
        }

#define CHOICE(name, ...)                                                                                              \
        static bool name (struct Parser *p, struct ParseResult *r)                                                     \
        {                                                                                                              \
                ParseFn args[]      = {__VA_ARGS__};                                                                   \
                struct ParseState s = saveParseState (p, r);                                                           \
                for (int i = 0; i < sizeof (args) / sizeof (*args); i++) {                                             \
                        if (args[i](p, r)) {                                                                           \
                                return true;                                                                           \
                        }                                                                                              \
                }                                                                                                      \
                restoreParseState (p, r, s);                                                                           \
                return false;                                                                                          \
        }

#define MANY(name, fn)                                                                                                 \
        static bool name (struct Parser *p, struct ParseResult *r)                                                     \
        {                                                                                                              \
                struct ParseState s = saveParseState (p, r);                                                           \
                while (fn (p, r))                                                                                      \
                        ;                                                                                              \
                return true;                                                                                           \
        }

#define N_TIMES(name, fn, amount)                                                                                      \
        static bool name (struct Parser *p, struct ParseResult *r)                                                     \
        {                                                                                                              \
                struct ParseState s = saveParseState (p, r);                                                           \
                for (int i = 0; i < (amount); i++)                                                                     \
                        if (!fn (p, r)) {                                                                              \
                                restoreParseState (p, r, s);                                                           \
                                return false;                                                                          \
                        }                                                                                              \
                return true;                                                                                           \
        }

#define ONE_OF(name, chars)                                                                                            \
        static bool name (struct Parser *p, struct ParseResult *r)                                                     \
        {                                                                                                              \
                return oneOf (p, chars, r);                                                                            \
        }

#define ZERO_OR_MORE(name, chars)                                                                                      \
        static bool name (struct Parser *p, struct ParseResult *r)                                                     \
        {                                                                                                              \
                return zeroOrMore (p, chars, r);                                                                       \
        }

#define ONE_OR_MORE(name, chars)                                                                                       \
        static bool name (struct Parser *p, struct ParseResult *r)                                                     \
        {                                                                                                              \
                return oneOrMore (p, chars, r);                                                                        \
        }

#define EXACTLY(name, chars)                                                                                           \
        static bool name (struct Parser *p, struct ParseResult *r)                                                     \
        {                                                                                                              \
                return exactly (p, chars, r);                                                                          \
        }

#define SUPPRESS(name, fn)                                                                                             \
        static bool name (struct Parser *p, struct ParseResult *r)                                                     \
        {                                                                                                              \
                suppressParseResult (r);                                                                               \
                bool result = fn (p, r);                                                                               \
                unsuppressParseResult (r);                                                                             \
                return result;                                                                                         \
        }

struct Parser {
        size_t index;
        char *buffer;
};

struct ParseResult {
        size_t size;
        size_t count;
        char *buffer;
        bool surpress;
};

struct ParseState {
        size_t index;
        size_t count;
};

struct FileEntry {
        int id;
        size_t line;
        mode_t permission;
        uid_t uid;
        char *ownerName;
        gid_t gid;
        char *groupName;
        char *filename;
        char temporaryFileName[12];
};

struct Config {
        int dryrun;
        char *testfile;
};

typedef bool (*ParseFn) (struct Parser *parser, struct ParseResult *result);

int permission_mask[] = {S_IRUSR, S_IWUSR, S_IXUSR, S_IRGRP, S_IWGRP, S_IXGRP, S_IROTH, S_IWOTH, S_IXOTH};

void *alloc (size_t size)
{
        void *mem = malloc (size);

        if (!mem) {
                perror ("alloc");
                exit (EXIT_FAILURE);
        }

        return mem;
}

char peek (struct Parser *parser)
{
        return parser->buffer[parser->index];
}

char advance (struct Parser *parser)
{
        if (peek (parser) == '\0')
                return '\0';
        return parser->buffer[parser->index++];
}

bool match (struct Parser *parser, char c)
{
        if (peek (parser) == c) {
                advance (parser);
                return true;
        }

        return false;
}

void initializeParseResult (struct ParseResult *result)
{
        result->buffer   = alloc (8 * sizeof (char));
        result->count    = 0;
        result->size     = 8;
        result->surpress = false;
}

void reallocParseResult (struct ParseResult *result)
{
        result->size *= 2;
        result->buffer = realloc (result->buffer, result->size * sizeof (char));

        if (!result->buffer) {
                perror ("realloc");
                exit (EXIT_FAILURE);
        }
}

struct ParseState saveParseState (struct Parser *parser, struct ParseResult *result)
{
        return (struct ParseState){.count = result->count, .index = parser->index};
}

void restoreParseState (struct Parser *parser, struct ParseResult *result, struct ParseState state)
{
        parser->index = state.index;
        result->count = state.count;
}

void resetParseResult (struct ParseResult *result)
{
        result->count = 0;
}

void suppressParseResult (struct ParseResult *result)
{
        result->surpress = true;
}

void unsuppressParseResult (struct ParseResult *result)
{
        result->surpress = false;
}

char *asString (struct ParseResult *result)
{
        result->buffer[result->count] = '\0';

        return result->buffer;
}

void writeChar (struct ParseResult *result, char c)
{
        if (result->surpress)
                return;

        if (result->count == result->size)
                reallocParseResult (result);

        result->buffer[result->count++] = c;
}

bool zeroOrMore (struct Parser *parser, char *chars, struct ParseResult *result)
{
        bool hasMatch = true;
        while (hasMatch) {
                hasMatch = false;
                for (char *c = chars; *c != '\0'; c++) {
                        if (match (parser, *c)) {
                                writeChar (result, *c);
                                hasMatch = true;
                        }
                }
        }

        return true;
}

bool oneOrMore (struct Parser *parser, char *chars, struct ParseResult *result)
{
        bool hasMatch = true;
        bool matches  = 0;
        while (hasMatch) {
                hasMatch = false;
                for (char *c = chars; *c != '\0'; c++) {
                        if (match (parser, *c)) {
                                writeChar (result, *c);
                                hasMatch = true;
                                matches += 1;
                        }
                }
        }

        return matches > 0;
}

bool oneOf (struct Parser *parser, char *chars, struct ParseResult *result)
{
        for (char *c = chars; *c != '\0'; c++)

                if (match (parser, *c)) {
                        writeChar (result, *c);
                        return true;
                }

        return false;
}

bool exactly (struct Parser *parser, char *value, struct ParseResult *result)
{
        struct ParseState s = saveParseState (parser, result);
        for (char *c = value; *c != '\0'; c++)
                if (match (parser, *c)) {
                        writeChar (result, *c);
                } else {
                        restoreParseState (parser, result, s);
                        return false;
                }
        return true;
}

ONE_OR_MORE (whitespace, "\t ")
MANY (whiteSpaceSeq, whitespace)
SUPPRESS (ignoreWhiteSpaceSeq, whiteSpaceSeq)

ONE_OR_MORE (integer, NUMERIC)
ONE_OR_MORE (newline, "\r\n")
SUPPRESS (ignoreNewLine, newline)

CHOICE (whiteSpaceOrNewLine, whitespace, newline)
MANY (whiteSpaceOrNewLineSeq, whiteSpaceOrNewLine)
SUPPRESS (ignoreWhiteSpaceNewLineSeq, whiteSpaceOrNewLineSeq)

ONE_OF (readPermissionBit, "r-")
ONE_OF (writePermissionBit, "w-")
ONE_OF (executePermissionBit, "x-")
SEQUENCE (permissionTriplet, readPermissionBit, writePermissionBit, executePermissionBit)
N_TIMES (permissionBits, permissionTriplet, 3)

ONE_OR_MORE (fileSafeChars, FILESAFE_CHARS);
EXACTLY (escapedSpace, "\\ ");
CHOICE (filenameSegment, fileSafeChars, escapedSpace)
MANY (filename, filenameSegment)

ONE_OR_MORE (ownerName, FILESAFE_CHARS);
ONE_OR_MORE (groupName, FILESAFE_CHARS);

bool atEnd (struct Parser *parser)
{
        return peek (parser) == '\0';
}

mode_t createPermissionMask (char *perms)
{
        mode_t permission = 0;

        for (int i = 0; i < 9; i++)
                if (perms[i] != '-')
                        permission |= permission_mask[i];

        return permission;
}

struct FileEntry parseFileEntry (struct Parser *parser, struct ParseResult *result)
{
        ignoreWhiteSpaceNewLineSeq (parser, result);

        struct FileEntry entry;

        if (!integer (parser, result)) {
                fprintf (stderr, "error: missing id in first column");
                exit (EXIT_FAILURE);
        }

        entry.id = strtol (asString (result), NULL, 10);

        ignoreWhiteSpaceSeq (parser, result);

        resetParseResult (result);
        ownerName (parser, result);
        char *owner     = asString (result);
        entry.ownerName = alloc ((strlen (owner) + 1) * sizeof (char));
        strcpy (entry.ownerName, owner);

        ignoreWhiteSpaceSeq (parser, result);

        resetParseResult (result);
        groupName (parser, result);
        char *group     = asString (result);
        entry.groupName = alloc ((strlen (group) + 1) * sizeof (char));
        strcpy (entry.groupName, group);

        ignoreWhiteSpaceSeq (parser, result);

        resetParseResult (result);
        permissionBits (parser, result);

        entry.permission = createPermissionMask (asString (result));

        ignoreWhiteSpaceSeq (parser, result);

        if (ignoreNewLine (parser, result)) {
                entry.filename = NULL;
                return entry;
        }

        resetParseResult (result);

        filename (parser, result);

        entry.filename = alloc ((strlen (asString (result)) + 1) * sizeof (char));
        strcpy (entry.filename, asString (result));

        ignoreWhiteSpaceSeq (parser, result);

        ignoreNewLine (parser, result);

        resetParseResult (result);

        return entry;
}

void destroyFileEntry (struct FileEntry *entry)
{
        if (entry->filename)
                free (entry->filename);

        if (entry->groupName)
                free (entry->groupName);

        if (entry->ownerName)
                free (entry->ownerName);
}

struct Parser createFileParser (FILE *fp)
{
        struct Parser parser;

        fseek (fp, 0, SEEK_END);

        size_t file_size = ftell (fp);

        rewind (fp);

        parser.buffer = alloc (file_size + 1);
        parser.index  = 0;

        fread (parser.buffer, file_size, 1, fp);

        parser.buffer[file_size] = '\0';

        return parser;
}

struct FileEntry *readDirectory (DIR *dirp, size_t *num_entries)
{
        struct dirent *d;

        size_t entries_size = 8, entries_count = 0;
        struct FileEntry *entries = alloc (entries_size * sizeof (struct FileEntry));

        while ((d = readdir (dirp)) != NULL) {
                if (entries_count == entries_size) {
                        entries_size *= 2;
                        entries = realloc (entries, entries_size * sizeof (struct FileEntry));

                        if (!entries) {
                                perror ("realloc");
                                exit (EXIT_FAILURE);
                        }
                }

                if (strcmp (d->d_name, ".") == 0)
                        continue;

                if (strcmp (d->d_name, "..") == 0)
                        continue;

                struct FileEntry *entry = &entries[entries_count++];
                entry->filename         = alloc ((strlen (d->d_name) + 1) * sizeof (char));

                strcpy (entry->filename, d->d_name);

                struct stat st;

                if (fstatat (dirfd (dirp), entry->filename, &st, 0) < 0) {
                        perror ("fstatat");
                        exit (EXIT_FAILURE);
                }

                entry->permission  = st.st_mode;
                entry->gid         = st.st_gid;
                entry->uid         = st.st_uid;

                struct passwd *pwd = getpwuid (entry->uid);
                struct group *grp  = getgrgid (entry->gid);

                entry->ownerName   = alloc ((strlen (pwd->pw_name) + 1) * sizeof (char));
                strcpy (entry->ownerName, pwd->pw_name);

                entry->groupName = alloc ((strlen (grp->gr_name) + 1) * sizeof (char));
                strcpy (entry->groupName, grp->gr_name);
        }

        *num_entries = entries_count;
        return entries;
}

void writeDirectoryListing (struct FileEntry *entries, size_t entries_count, char *filepath)
{
        int tmpfd = mkstemp (filepath);

        if (tmpfd < 0) {
                perror ("mkstemp");
                exit (EXIT_FAILURE);
        }

        FILE *fp = fdopen (tmpfd, "w");

        if (!fp) {
                perror ("fopen");
                exit (EXIT_FAILURE);
        }

        for (int entry_idx = 0; entry_idx < entries_count; entry_idx++) {
                struct FileEntry *current_entry = &entries[entry_idx];
                char rwx[]                      = {'r', 'w', 'x'};

                fprintf (fp, "%d\t", entry_idx);

                struct passwd *pwd = getpwuid (current_entry->uid);
                struct group *grp  = getgrgid (current_entry->gid);

                fprintf (fp, "%s\t", pwd->pw_name);

                fprintf (fp, "%s\t", grp->gr_name);

                for (int i = 0; i < 9; i++)
                        if (current_entry->permission & permission_mask[i])
                                fputc (rwx[i % 3], fp);
                        else
                                fputc ('-', fp);

                fputc ('\t', fp);
                fwrite (current_entry->filename, strlen (current_entry->filename), 1, fp);
                fputc ('\n', fp);
        }
        fclose (fp);
}

void launchAndWaitForEditor (char *filepath)
{
        char *editor = getenv ("EDITOR");

        if (!editor) {
                fprintf (stderr, "Please set an EDITOR.\n");
                exit (EXIT_FAILURE);
        }
        pid_t editor_pid = fork ();

        if (editor_pid == 0) {
                execlp (editor, editor, filepath, NULL);
                perror ("execlp");
                exit (EXIT_FAILURE);
        }
        int status;

        waitpid (editor_pid, &status, 0);

        if (!WIFEXITED (status)) {
                printf ("error: editor exited abnormally.\n");
                exit (EXIT_FAILURE);
        }
}

void openEditor (struct Config *config, char *dir)
{
        DIR *dirp = opendir (dir);

        if (!dirp) {
                perror ("opendir");
                exit (EXIT_FAILURE);
        }

        size_t entries_count;

        struct FileEntry *entries = readDirectory (dirp, &entries_count);
        char filepath[]           = "/tmp/wdiredXXXXXX";

        writeDirectoryListing (entries, entries_count, filepath);
        launchAndWaitForEditor (filepath);

        FILE *fp             = fopen (filepath, "r");
        struct Parser parser = createFileParser (fp);
        fclose (fp);
        unlink (filepath);

        struct ParseResult result;
        initializeParseResult (&result);

        struct FileEntry *new_entries = alloc (entries_count * sizeof (struct FileEntry));

        for (int entry_idx = 0; entry_idx < entries_count; entry_idx++)
                new_entries[entry_idx] = parseFileEntry (&parser, &result);

        if (config->dryrun)
                fprintf (stderr, "Dry Run:\n");

        for (int entry_idx = 0; entry_idx < entries_count; entry_idx++) {
                struct FileEntry *old_entry = &entries[entry_idx];
                struct FileEntry *new_entry = &new_entries[entry_idx];

                if (!new_entry->filename) {
                        if (config->dryrun)
                                fprintf (stderr, "delete: %s\n", old_entry->filename);
                        else if (unlinkat (dirfd (dirp), old_entry->filename, 0) < 0)
                                perror ("unlinkat");

                        continue;
                } else if (strcmp (new_entry->filename, old_entry->filename) != 0) {
                        if (config->dryrun)
                                fprintf (stderr, "rename: %s -> %s\n", old_entry->filename, new_entry->filename);
                        else if (renameat (dirfd (dirp), old_entry->filename, dirfd (dirp), new_entry->filename) < 0)
                                perror ("renameat");
                }

                if ((old_entry->permission & 0777) != new_entry->permission) {
                        if (config->dryrun) {
                                fprintf (stderr,
                                         "%s permission: %o -> %o\n",
                                         new_entry->filename,
                                         old_entry->permission & 0777,
                                         new_entry->permission);
                        } else if (fchmodat (dirfd (dirp), new_entry->filename, new_entry->permission, 0) < 0)
                                perror ("fchmodat");
                }

                if (strcmp (old_entry->ownerName, new_entry->ownerName) != 0 ||
                    strcmp (old_entry->groupName, new_entry->groupName) != 0) {
                        fchownat (dirfd (dirp), new_entry->filename, new_entry->uid, new_entry->gid, 0);
                }

                destroyFileEntry (new_entry);
        }

        closedir (dirp);
}

int main (int argc, char **argv)
{
        struct Config config     = {.dryrun = 0, .testfile = NULL};
        struct option longopts[] = {
                {.name = "dryrun", .flag = &config.dryrun, .val = 1},
                {.name = "test", .flag = NULL, .val = 'f', .has_arg = required_argument},
                {NULL}
        };
        int optidx = 0, c;
        while ((c = getopt_long (argc, argv, "f:", longopts, &optidx)) != -1) {
                switch (c) {
                case 0: continue;
                case 'f': config.testfile = optarg; break;
                default: break;
                }
        }

        if (config.testfile) {
                FILE *fp = fopen (config.testfile, "r");

                if (!fp) {
                        perror ("fopen");
                        exit (EXIT_FAILURE);
                }

                struct Parser parser = createFileParser (fp);
                struct ParseResult result;
                initializeParseResult (&result);
                for (int i = 0; i < 5; i++)
                        parseFileEntry (&parser, &result);

        } else {
                if (optind >= argc)
                        openEditor (&config, ".");
                else
                        openEditor (&config, argv[optind]);
        }
}
