/***************************************************************************
 *   Copyright (C) 2020 by Federico Amedeo Izzo IU2NUO,                    *
 *                         Niccolò Izzo IU2KIN                             *
 *                         Frederik Saraci IU2NRO                          *
 *                         Silvano Seva IU2KWO                             *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, see <http://www.gnu.org/licenses/>   *
 ***************************************************************************/


#include "emulator.h"
#include "sdl_engine.h"

#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <interfaces/keyboard.h>
#include <SDL2/SDL.h>

#include <readline/readline.h>
#include <readline/history.h>

/* Custom SDL Event to request a screenshot */
extern Uint32 SDL_Screenshot_Event;

radio_state Radio_State = {12, 8.2f, 3, 4, 1, false, false};

typedef int (*_climenu_fn)(void *self, int argc, char **argv);

typedef struct
{
    char *name;
    char *description;
    void *var;
    _climenu_fn fn;
} _climenu_option;

enum shell_retvals
{
    SH_ERR = -1,
    SH_CONTINUE = 0,
    SH_WHAT = 1,
    SH_EXIT_OK = 2,
};


keyboard_t _shellkeyq[25] = {0};
int _skq_cap = 25;
int _skq_head;
int _skq_tail;
int _skq_in;
int _skq_out;
void _dump_skq()
{
    for(int i = 0; i < _skq_cap; i++)
    {
        printf("skq[%d] == %d\n", i, _shellkeyq[i]);
    }
}
void shellkeyq_put(keyboard_t keys)
{
    // note - we must allow keys == 0 to be inserted because otherwise a queue
    // full of [1,1,1,1,1] is simulating HOLDING 1, and we sometimes
    // (well, often) want [1,0,1,0,1,0] to simulate separate keypresses
    // this, of course, relies on the kbd_thread getting just one element off
    // the queue for every kbd_getKeys().
    if(_skq_in > _skq_out + _skq_cap)
    {
        printf("too many keys!\n");
        return;
    }

    _shellkeyq[ _skq_tail ] = keys;
    _skq_in++;
    _skq_tail = (_skq_tail + 1) % _skq_cap;
}
keyboard_t shellkeyq_get()
{
    if(_skq_in > _skq_out)
    {
        //only if we've fallen behind and there's data in there:
        keyboard_t out = _shellkeyq[ _skq_head ];
        _shellkeyq[ _skq_head ] = 0;
        _skq_out++;
        _skq_head = (_skq_head + 1) % _skq_cap;
        return out;
    }
    else
    {
        return 0; //no keys
    }
}
void _test_skq()
{
    for(int i = 0; i < 257; i++)
    {
        shellkeyq_put(i + 1);
    }

    //clear it out now
    while(shellkeyq_get());
}


int shell_ready(void *_self, int _argc, char **_argv)
{
    (void) _self;
    (void) _argc;
    (void) _argv;

    while(_skq_in > _skq_out)
    {
        usleep(10 * 1000); //sleep until keyboard is caught up
    }
    return SH_CONTINUE;
}

keyboard_t keyname2keyboard(char *name)
{
    /*  The line noise at the end of this comment is a vim macro for taking the
     *  keyboard.h interface and putting it into the format further below.
     *  You can load it into vim register k with "kyy and run the macro with @k
     *  (and then you can repeat a macro register application with @@ )
     *  (substitute k with any register you like)
     *  Once you've got all the names quoted, you can J them all together into
     *  a nice block.
     *
     *   _i"ElC",
     *
     */
    char *names[] =
    {
        "KEY_0", "KEY_1", "KEY_2", "KEY_3", "KEY_4", "KEY_5", "KEY_6", "KEY_7",
        "KEY_8", "KEY_9", "KEY_STAR", "KEY_HASH", "KEY_ENTER", "KEY_ESC",
        "KEY_UP","KEY_DOWN", "KEY_LEFT", "KEY_RIGHT", "KEY_MONI", "KEY_F1",
        "KEY_F2", "KEY_F3", "KEY_F4", "KEY_F5", "KEY_F6", "KEY_F7", "KEY_F8",
        "KNOB_LEFT", "KNOB_RIGHT",
    };

    int numnames = sizeof(names) / sizeof(char *);

    for(int i = 0; i < numnames; i++)
    {
       /*
        * +4 to skip the KEY_ on all the names, non +4 to allow for KNOB_LEFT.
        * This also means you can write KEY_LEFT as "KEY_LEFT", or "LEFT" and
        * KNOB_LEFT as "KNOB_LEFT" or "_LEFT"
        *
        * so if name == "2", this whole function will return equivalent to KEY_2 cpp define
        * and if name=="LEFT", then you get equivalent to KEY_LEFT cpp define
        * and if name=="_LEFT", then you get equivalent to KNOB_LEFT cpp define
        * and if name=="KNOB_LEFT", then you get equivalent to KNOB_LEFT cpp define
        * and if name=="KEY_2", then you get equivalent to KEY_2 cpp define.
        *
        * Of course order matters a great deal in names array, has to match the
        * bit field generated in interface/keyboard.h so double check that with
        * every update
        */

        if((strcasecmp(name, names[i] + 4) == 0) ||
           (strcasecmp(name, names[i]) == 0))       //notice case insensitive
        {
            return (1 << i);
        }
    }
    return 0;
}

int pressKey(void *_self, int _argc, char **_argv)
{
    (void) _self;

    printf("Press Keys: [\n");
    keyboard_t last = 0;

    for(int i = 0; i < _argc; i++)
    {
        if(_argv[i] != NULL)
        {
            printf("\t%s, \n", _argv[i]);
            keyboard_t press = keyname2keyboard(_argv[i]);
            if(press == last)
            {
                /* otherwise if you send key ENTER DOWN DOWN DOWN DOWN DOWN
                 * it will just hold DOWN for (5/(kbd_task_hz)) seconds
                 * so we need to give it a 0 value to get a 'release'
                 * so the next input is recognized as separate
                 * we only need to do this if we have two identical keys back
                 * to back, because keyboard_t will have a zero for this key's
                 * flag on other keys, which gives us the release we need
                 */
                shellkeyq_put(0);
            }
            shellkeyq_put(press);
            last = press;
        }
    }
    printf("\t]\n");
    shell_ready(NULL, 0, NULL);
    return SH_CONTINUE; // continue
}

// pressMultiKeys allows for key combos by sending all the keys specified in
// one keyboard_t
int pressMultiKeys(void *_self, int _argc, char **_argv)
{
    (void) _self;
    printf("Press Keys: [\n");
    keyboard_t combo = 0;

    for(int i = 0; i < _argc; i++)
    {
        if(_argv[i] != NULL)
        {
            printf("\t%s, \n", _argv[i]);
            combo |= keyname2keyboard(_argv[i]);
        }
    }

    shellkeyq_put(combo);
    printf("\t]\n");
    shell_ready(NULL, 0, NULL);
    return SH_CONTINUE; // continue
}

int template(void *_self, int _argc, char **_argv)
{
    _climenu_option *self = (_climenu_option *) _self;
    printf("%s\n\t%s\n", self->name, self->description);

    for(int i = 0; i < _argc; i++)
    {
        if(_argv[i] != NULL)
        {
            printf("\tArgs:\t%s\n", _argv[i]);
        }
    }

    return SH_CONTINUE; // continue
}

int screenshot(void *_self, int _argc, char **_argv)
{
    (void) _self;
    char *filename = "screenshot.bmp";

    if(_argc && _argv[0] != NULL)
    {
        filename = _argv[0];
    }

    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_Screenshot_Event;
    e.user.data1 = malloc(sizeof(filename));
    strcpy(e.user.data1, filename);

    return SDL_PushEvent(&e) == 1 ? SH_CONTINUE : SH_ERR;
}

int setFloat(void *_self, int _argc, char **_argv)
{
    _climenu_option *self = (_climenu_option *) _self;

    if(_argc <= 0 || _argv[0] == NULL)
    {
        printf("%s is %f\n", self->name,  *(float *)(self->var));
    }
    else
    {
        sscanf(_argv[0], "%f", (float *)self->var);
        printf("%s is %f\n", self->name,  *(float *)(self->var));
    }

    return SH_CONTINUE; // continue

}

int toggleVariable(void *_self, int _argc, char **_argv)
{
    (void) _argc;
    (void) _argv;
    _climenu_option *self = (_climenu_option *) _self;
    *(int *)self->var = ! *(int *)self->var; //yeah, maybe this got a little out of hand

    return SH_CONTINUE; // continue
}

int shell_sleep(void *_self, int _argc, char **_argv)
{
    (void) _self;

    if(! _argc || _argv[0] == NULL)
    {
        printf("Provide a number in milliseconds to sleep as an argument\n");
        return SH_ERR;
    }

    useconds_t sleepus = atoi(_argv[0]) * 1000;
    usleep(sleepus);
    return SH_CONTINUE;
}

int shell_quit( void *_self, int _argc, char **_argv)
{
    (void) _self;
    (void) _argc;
    (void) _argv;
    printf("QUIT: 73!\n");

    //could remove history entries here, if we wanted
    return SH_EXIT_OK; //normal quit
}

int printState( void *_self, int _argc, char **_argv)
{
    (void) _self;
    (void) _argc;
    (void) _argv;
    printf("\nCurrent state\n");
    printf("RSSI   : %f\n", Radio_State.RSSI);
    printf("Battery: %f\n", Radio_State.Vbat);
    printf("Mic    : %f\n", Radio_State.micLevel);
    printf("Volume : %f\n", Radio_State.volumeLevel);
    printf("Channel: %f\n", Radio_State.chSelector);
    printf("PTT    : %s\n\n", Radio_State.PttStatus ? "true" : "false");
    return SH_CONTINUE;
}

int shell_nop( void *_self, int _argc, char **_argv)
{
    (void) _self;
    (void) _argc;
    (void) _argv;
    //do nothing! what it says on the tin
    return SH_CONTINUE;
}

// Forward declaration needed to include function pointer in the table below
int shell_help( void *_self, int _argc, char **_argv);

_climenu_option _options[] =
{
    /* name/shortcut   description            var reference, if available    method to call */
    {"rssi",    "Set rssi",     (void *) &Radio_State.RSSI,        setFloat },
    {"vbat",    "Set vbat",     (void *) &Radio_State.Vbat,        setFloat },
    {"mic",     "Set miclevel", (void *) &Radio_State.micLevel,    setFloat },
    {"volume",  "Set volume",   (void *) &Radio_State.volumeLevel, setFloat },
    {"channel", "Set channel",  (void *) &Radio_State.chSelector,  setFloat },
    {"ptt",     "Toggle PTT",   (void *) &Radio_State.PttStatus,   toggleVariable },
    {"key",     "Press keys in sequence (e.g. 'key ENTER DOWN ENTER' will descend through two menus)",
                                NULL,   pressKey
    },
    {"keycombo", "Press a bunch of keys simultaneously", NULL, pressMultiKeys },
    {"show",     "Show current radio state (ptt, rssi, etc)", NULL, printState},
    {"screenshot", "[screenshot.bmp] Save screenshot to first arg or screenshot.bmp if none given",
                                NULL,   screenshot
    },
    {"sleep",   "Wait some number of ms",           NULL,   shell_sleep },
    {"help",    "Print this help",                  NULL,   shell_help },
    {"nop",     "Do nothing (useful for comments)", NULL,   shell_nop},
    {"quit",    "Quit, close the emulator",         NULL,   shell_quit },
    /*{"ready",     */
    /*"Wait until ready. Currently supports keyboard, so will wait until all keyboard events are processed,"*/
    /*"but is already implied by key and keycombo so there's not much direct use for it right now",*/
    /*NULL,   shell_ready },*/
};
int num_options = (sizeof(_options) / sizeof(_climenu_option));

int shell_help( void *_self, int _argc, char **_argv)
{
    (void) _self;
    (void) _argc;
    (void) _argv;
    printf("OpenRTX emulator shell\n\n");

    for(int i = 0; i < num_options; i++)
    {
        _climenu_option *o = &_options[i];
        printf("%10s -> %s\n", o->name, o->description);
    }

    return SH_CONTINUE;
}


_climenu_option *findMenuOption(char *tok)
{
    for(int i = 0; i < num_options; i++)
    {
        _climenu_option *o = &_options[i];

        /*
         * strncmp like this allows for typing shortcuts like just "r" instead
         * of the full "rssi". Priority for conflicts (like if there's "s"
         * which could mean either "show" or "screenshot" ) is set by ordering
         * in the _options array
         */
        if(strncmp(tok, o->name, strlen(tok)) == 0)
        {
            return o;
        }
    }

    return NULL;
}

void striptoken(char *token)
{
    for(size_t i = 0; i < strlen(token); i++)
    {
        if(token[i] == '\n')
        {
            token[i] = 0;
        }
    }
}

int process_line(char *line)
{
    char *token = strtok(line, " ");

    if(token == NULL)
    {
        return SH_ERR;
    }

    striptoken(token);
    _climenu_option *o = findMenuOption(token);
    char *args[12] = {NULL};
    int i = 0;

    for(i = 0; i < 12; i++)
    {
        //immediately strtok again since first is a command rest are args
        token = strtok(NULL, " ");
        if(token == NULL)
        {
            break;
        }
        striptoken(token);
        args[i] = token;
    }

    if(token != NULL)
    {
        printf("\nGot too many arguments, args truncated \n");
    }

    if(o != NULL)
    {
        if(o->fn != NULL)
        {
            return o->fn(o, i, args);
        }
        else
        {
            printf("Bad fn for o, check option array for bad data\n");
            return SH_ERR;
        }
    }
    else
    {
        return SH_WHAT; //not understood
    }
}

void *startCLIMenu()
{
    printf("\n\n");
    char *histfile = ".emulatorsh_history";
    shell_help(NULL, 0, NULL);
    int ret = SH_CONTINUE;
    using_history();
    read_history(histfile);

    do
    {
        char *r = readline(">");
        if(r == NULL)
        {
            ret = SH_EXIT_OK;
        }
        else if(strlen(r) > 0)
        {
            add_history(r);
            ret = process_line(r);
        }
        else
        {
            ret = SH_CONTINUE;
        }

        switch(ret)
        {
            default:
                fflush(stdout);
                break;

            case SH_WHAT:
                printf("?\n(type h or help for help)\n");
                ret = SH_CONTINUE;
                /*printf("\n>");*/
                break;

            case SH_CONTINUE:
                /*printf("\n>");*/
                break;

            case SH_EXIT_OK:
                //normal quit
                break;

            case SH_ERR:
                //error
                printf("Error running that command\n");
                ret = SH_CONTINUE;
                break;
        }
        free(r); //free the string allocated by readline
    }
    while(ret == SH_CONTINUE);

    fflush(stdout);
    write_history(histfile);
    Radio_State.PowerOff = true;
}

void emulator_start()
{
    init_sdl();

    pthread_t cli_thread;
    int err = pthread_create(&cli_thread, NULL, startCLIMenu, NULL);

    if(err)
    {
        printf("An error occurred starting the emulator CLI thread: %d\n", err);
    }
}
