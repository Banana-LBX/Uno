#include <notcurses/notcurses.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef enum { C_RED, C_YELLOW, C_GREEN, C_BLUE, C_WILD, C_NONE } Color;

typedef enum {
    T_NUM, T_SKIP, T_REVERSE, T_DRAW2, T_WILD, T_WILD4
} Type;

typedef struct {
    Color color;
    Type  type;
    int   num;
} Card;

#define MAX_DECK   108
#define MAX_HAND   40
#define NPLAYERS   4

typedef struct {
    char name[16];
    Card hand[MAX_HAND];
    int  n; // number of cards in hand
    int  is_human;
} Player;

static Card   deck[MAX_DECK];
static int    deck_n = 0;
static Card   discard[MAX_DECK];
static int    discard_n = 0;

static Player players[NPLAYERS];
static int    cur = 0; // index of current player
static int    dir = 1; // 1 clockwise -1 counterclockwise
static Color  active_color = C_NONE;

static struct notcurses* nc = NULL;
static struct ncplane*   std = NULL;

static char msg[256] = "";

// Helpers
static const char* color_name(Color c) {
    switch (c) {
        case C_RED:    return "RED";
        case C_YELLOW: return "YELLOW";
        case C_GREEN:  return "GREEN";
        case C_BLUE:   return "BLUE";
        case C_WILD:   return "WILD";
        default:       return "?";
    }
}

static void color_rgb(Color c, unsigned* r, unsigned* g, unsigned* b) {
    switch (c) {
        case C_RED:    *r = 0xe0; *g = 0x30; *b = 0x30; break;
        case C_YELLOW: *r = 0xe0; *g = 0xd0; *b = 0x20; break;
        case C_GREEN:  *r = 0x20; *g = 0xb0; *b = 0x40; break;
        case C_BLUE:   *r = 0x30; *g = 0x60; *b = 0xe0; break;
        case C_WILD:   *r = 0xd0; *g = 0xd0; *b = 0xd0; break;
        default:       *r = 0x80; *g = 0x80; *b = 0x80; break;
    }
}

static const char* type_label(Type t) {
    switch (t) {
        case T_SKIP:    return "SKIP";
        case T_REVERSE: return "REV";
        case T_DRAW2:   return "+2";
        case T_WILD:    return "WILD";
        case T_WILD4:   return "+4W";
        default:        return "";
    }
}

static void card_center_label(const Card* c, char* out, size_t outsz) {
    if (c->type == T_NUM) snprintf(out, outsz, "%d", c->num);
    else                  snprintf(out, outsz, "%s", type_label(c->type));
}

// Deck construction / shuffling
static void push_deck(Card c) { deck[deck_n++] = c; }

static void build_deck(void) {
    deck_n = 0;
    Color cols[4] = { C_RED, C_YELLOW, C_GREEN, C_BLUE };
    for (int ci = 0; ci < 4; ci++) {
        Color col = cols[ci];
        push_deck((Card){col, T_NUM, 0});
        for (int n = 1; n <= 9; n++) {
            push_deck((Card){col, T_NUM, n});
            push_deck((Card){col, T_NUM, n});
        }
        for (int k = 0; k < 2; k++) {
            push_deck((Card){col, T_SKIP, 0});
            push_deck((Card){col, T_REVERSE, 0});
            push_deck((Card){col, T_DRAW2, 0});
        }
    }
    for (int k = 0; k < 4; k++) {
        push_deck((Card){C_WILD, T_WILD, 0});
        push_deck((Card){C_WILD, T_WILD4, 0});
    }
}

static void shuffle_deck(void) {
    for (int i = deck_n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        Card t = deck[i]; deck[i] = deck[j]; deck[j] = t;
    }
}

// reshuffle discard pile back into deck when it empties
static void reshuffle_if_needed(void) {
    if (deck_n > 0) return;
    if (discard_n <= 1) return;
    Card top = discard[discard_n - 1];
    for (int i = 0; i < discard_n - 1; i++) deck[deck_n++] = discard[i];
    discard_n = 1;
    discard[0] = top;
    shuffle_deck();
    snprintf(msg, sizeof msg, "Deck reshuffled from discard pile.");
}

static Card draw_card(void) {
    reshuffle_if_needed();
    if (deck_n == 0) {
        // fallback to build a fresh deck
        build_deck();
        shuffle_deck();
    }
    return deck[--deck_n];
}

static void give_card(Player* p, Card c) {
    if (p->n < MAX_HAND) p->hand[p->n++] = c;
}

static void remove_card(Player* p, int idx) {
    for (int i = idx; i < p->n - 1; i++) p->hand[i] = p->hand[i + 1];
    p->n--;
}

// Game rules
static int card_playable(const Card* c, Color top_color, const Card* top) {
    if (c->type == T_WILD || c->type == T_WILD4) return 1;
    if (c->color == top_color) return 1;
    if (c->type == T_NUM && top->type == T_NUM && c->num == top->num) return 1;
    if (c->type != T_NUM && c->type == top->type) return 1;
    return 0;
}

static int next_player(int from) {
    int n = from + dir;
    if (n < 0) n += NPLAYERS;
    if (n >= NPLAYERS) n -= NPLAYERS;
    return n;
}

/* Very simple UNO bot: play first legal card, preferring
 * non-wild cards, and preferring color that leaves it holding
 * the most cards of one color afterward. Picks the majority
 * color in its hand when forced to declare a wild color. */
static Color ai_choose_color(Player* p) {
    int counts[4] = {0,0,0,0};
    for (int i = 0; i < p->n; i++) {
        if (p->hand[i].color <= C_BLUE) counts[p->hand[i].color]++;
    }
    int best = 0;
    for (int i = 1; i < 4; i++) if (counts[i] > counts[best]) best = i;
    return (Color)best;
}

static int ai_pick_card(Player* p, Color top_color, const Card* top) {
    int best = -1;
    for (int i = 0; i < p->n; i++) {
        if (card_playable(&p->hand[i], top_color, top)) {
            if (p->hand[i].type != T_WILD && p->hand[i].type != T_WILD4) return i;
            if (best == -1) best = i;
        }
    }
    return best;
}

// Rendering
#define CARD_W 9
#define CARD_H 5

static void draw_card_face(int y, int x, const Card* c, int highlight) {
    unsigned r, g, b;
    color_rgb(c->color, &r, &g, &b);
    char label[8];
    card_center_label(c, label, sizeof label);

    const char* fillA = "▓";
    const char* fillB = "▒";
    const char* edge  = "█";

    /* border color: white if highlighted (selected), else card color */
    for (int row = 0; row < CARD_H; row++) {
        ncplane_set_fg_rgb8(std, r, g, b);
        if (highlight) ncplane_set_fg_rgb8(std, 255, 255, 255);
        ncplane_putstr_yx(std, y + row, x, "");
        for (int col = 0; col < CARD_W; col++) {
            const char* ch;
            if (row == 0 || row == CARD_H - 1 || col == 0 || col == CARD_W - 1)
                ch = edge;
            else if ((row + col) % 2 == 0)
                ch = fillA;
            else
                ch = fillB;
            ncplane_putstr_yx(std, y + row, x + col, ch);
        }
    }
    // center label
    int lx = x + (CARD_W - (int)strlen(label)) / 2;
    int ly = y + CARD_H / 2;
    ncplane_set_fg_rgb8(std, 20, 20, 20);
    ncplane_set_bg_rgb8(std, r, g, b);
    for (int col = -1; col <= (int)strlen(label); col++)
        ncplane_putstr_yx(std, ly, lx + col, " ");
    ncplane_set_fg_rgb8(std, 255, 255, 255);
    ncplane_putstr_yx(std, ly, lx, label);
    ncplane_set_bg_default(std);

    // small corner thingies
    char pip[2] = { color_name(c->color)[0], 0 };
    ncplane_set_fg_rgb8(std, r, g, b);
    ncplane_set_bg_rgb8(std, 20, 20, 20);
    ncplane_putstr_yx(std, y + 1, x + 1, pip);
    ncplane_putstr_yx(std, y + CARD_H - 2, x + CARD_W - 2, pip);
    ncplane_set_bg_default(std);
}

// back of the card (used for opponents and deck)
static void draw_card_back(int y, int x) {
    for (int row = 0; row < CARD_H; row++) {
        for (int col = 0; col < CARD_W; col++) {
            const char* ch;
            if (row == 0 || row == CARD_H - 1 || col == 0 || col == CARD_W - 1)
                ch = "█";
            else if ((row * 3 + col) % 3 == 0)
                ch = "▓";
            else if ((row + col) % 2 == 0)
                ch = "▒";
            else
                ch = "░";
            ncplane_set_fg_rgb8(std, 120, 40, 200);
            ncplane_putstr_yx(std, y + row, x + col, ch);
        }
    }
    ncplane_set_fg_rgb8(std, 255, 255, 255);
    ncplane_putstr_yx(std, y + CARD_H / 2, x + CARD_W / 2 - 1, "UNO");
}

static void set_status(const char* s) {
    snprintf(msg, sizeof msg, "%s", s);
}

static void render(int sel) {
    ncplane_erase(std);
    unsigned dimy, dimx;
    ncplane_dim_yx(std, &dimy, &dimx);

    // Title bar
    ncplane_set_fg_rgb8(std, 255, 255, 255);
    ncplane_set_bg_rgb8(std, 40, 40, 40);
    for (unsigned x = 0; x < dimx; x++) ncplane_putstr_yx(std, 0, x, " ");
    ncplane_putstr_yx(std, 0, 2, "UNO  —  4 Players");
    ncplane_set_bg_default(std);

    // Opponents (top row)
    int slots_x[3] = { 2, (int)dimx/2 - CARD_W/2, (int)dimx - CARD_W - 12 };
    const char* labels[3] = { "P2", "P3", "P4" };
    for (int i = 0; i < 3; i++) {
        Player* p = &players[i + 1];
        int x = slots_x[i];
        int y = 2;
        draw_card_back(y, x);
        ncplane_set_fg_rgb8(std, 255, 255, 0);
        char buf[64];
        snprintf(buf, sizeof buf, "%s %s: %d card%s%s",
                 (cur == i + 1) ? ">" : " ",
                 labels[i], p->n, p->n == 1 ? "" : "s",
                 p->n == 1 ? "  UNO!" : "");
        ncplane_set_fg_rgb8(std, cur == i + 1 ? 255 : 180,
                                  cur == i + 1 ? 255 : 180,
                                  cur == i + 1 ? 0   : 180);
        ncplane_putstr_yx(std, y + CARD_H, x - 1, buf);
    }

    // Middle: deck + discard + color indicator
    int midy = 9;
    int deckx = (int)dimx/2 - CARD_W - 4;
    int discx = (int)dimx/2 + 4;

    draw_card_back(midy, deckx);
    ncplane_set_fg_rgb8(std, 200, 200, 200);
    char dbuf[32];
    snprintf(dbuf, sizeof dbuf, "Deck: %d", deck_n);
    ncplane_putstr_yx(std, midy + CARD_H, deckx + 1, dbuf);

    if (discard_n > 0) {
        Card top = discard[discard_n - 1];
        Card shown = top;
        if (shown.color == C_WILD) shown.color = active_color; /* tint by chosen color */
        draw_card_face(midy, discx, &shown, 0);
    }
    unsigned cr, cg, cb;
    color_rgb(active_color, &cr, &cg, &cb);
    ncplane_set_fg_rgb8(std, cr, cg, cb);
    char cbuf[32];
    snprintf(cbuf, sizeof cbuf, "Color: %s", color_name(active_color));
    ncplane_putstr_yx(std, midy + CARD_H, discx, cbuf);

    // direction arrow
    ncplane_set_fg_rgb8(std, 255, 255, 255);
    ncplane_putstr_yx(std, midy + 2, (int)dimx/2 - 1, dir == 1 ? "→" : "←");

    // status line
    ncplane_set_fg_rgb8(std, 0, 255, 255);
    ncplane_putstr_yx(std, midy + CARD_H + 2, 2, msg);

    // player hand
    Player* me = &players[0];
    int y = (int)dimy - CARD_H - 2;
    int total_w = me->n * (CARD_W + 1);
    int startx = ((int)dimx - total_w) / 2;
    if (startx < 1) startx = 1;
    for (int i = 0; i < me->n; i++) {
        int x = startx + i * (CARD_W + 1);
        int hl = (cur == 0 && i == sel);
        draw_card_face(y, x, &me->hand[i], hl);
        if (hl) {
            ncplane_set_fg_rgb8(std, 255, 255, 0);
            ncplane_putstr_yx(std, y - 1, x + CARD_W/2, "▼");
        }
    }
    ncplane_set_fg_rgb8(std, 200, 200, 200);
    char hbuf[64];
    snprintf(hbuf, sizeof hbuf, "You (P1): %d card%s%s",
             me->n, me->n == 1 ? "" : "s", me->n == 1 ? "  UNO!" : "");
    ncplane_putstr_yx(std, y - 2, startx, hbuf);

    /* --- help line --- */
    ncplane_set_fg_rgb8(std, 130, 130, 130);
    ncplane_putstr_yx(std, (int)dimy - 1, 2,
        "←/→ select   Enter/Space play   d draw   q quit");

    ncplane_set_fg_default(std);
    notcurses_render(nc);
}

// Wild card color picker
static Color human_pick_color(void) {
    Color opts[4] = { C_RED, C_YELLOW, C_GREEN, C_BLUE };
    int sel = 0;
    for (;;) {
        ncplane_erase(std);
        unsigned dimy, dimx;
        ncplane_dim_yx(std, &dimy, &dimx);
        ncplane_set_fg_rgb8(std, 255, 255, 255);
        ncplane_putstr_yx(std, (int)dimy/2 - 3, (int)dimx/2 - 10, "Choose a color:");
        for (int i = 0; i < 4; i++) {
            unsigned r,g,b; color_rgb(opts[i], &r, &g, &b);
            int x = (int)dimx/2 - 16 + i * 8;
            int y = (int)dimy/2;
            ncplane_set_fg_rgb8(std, r, g, b);
            if (i == sel) ncplane_set_bg_rgb8(std, 60, 60, 60);
            ncplane_putstr_yx(std, y, x, "  ███  ");
            ncplane_putstr_yx(std, y + 1, x, "  ███  ");
            ncplane_set_bg_default(std);
            ncplane_putstr_yx(std, y + 2, x, color_name(opts[i]));
        }
        notcurses_render(nc);

        ncinput ni;
        uint32_t id = notcurses_get_blocking(nc, &ni);
        if (ni.evtype == NCTYPE_RELEASE) continue;
        if (id == NCKEY_LEFT || id == 'h') sel = (sel + 3) % 4;
        else if (id == NCKEY_RIGHT || id == 'l') sel = (sel + 1) % 4;
        else if (id == NCKEY_ENTER || id == ' ') return opts[sel];
        else if (id >= '1' && id <= '4') return opts[id - '1'];
    }
}

// Play a card, for both human and bots
static void play_card(Player* p, int idx, Color chosen_wild_color) {
    Card c = p->hand[idx];
    remove_card(p, idx);
    discard[discard_n++] = c;

    if (c.type == T_WILD || c.type == T_WILD4) active_color = chosen_wild_color;
    else active_color = c.color;

    int nxt = next_player(cur);

    switch (c.type) {
        case T_SKIP:
            snprintf(msg, sizeof msg, "%s played SKIP on %s.", p->name, players[nxt].name);
            nxt = next_player(nxt);
            break;
        case T_REVERSE:
            dir = -dir;
            snprintf(msg, sizeof msg, "%s played REVERSE.", p->name);
            nxt = next_player(cur);
            break;
        case T_DRAW2: {
            Player* victim = &players[nxt];
            give_card(victim, draw_card());
            give_card(victim, draw_card());
            snprintf(msg, sizeof msg, "%s played +2 — %s draws 2 and is skipped.",
                     p->name, victim->name);
            nxt = next_player(nxt);
            break;
        }
        case T_WILD4: {
            Player* victim = &players[nxt];
            give_card(victim, draw_card());
            give_card(victim, draw_card());
            give_card(victim, draw_card());
            give_card(victim, draw_card());
            snprintf(msg, sizeof msg, "%s played WILD+4 (%s) — %s draws 4 and is skipped.",
                     p->name, color_name(chosen_wild_color), victim->name);
            nxt = next_player(nxt);
            break;
        }
        case T_WILD:
            snprintf(msg, sizeof msg, "%s played WILD, chose %s.",
                     p->name, color_name(chosen_wild_color));
            break;
        default:
            snprintf(msg, sizeof msg, "%s played %s%s.",
                     p->name, color_name(c.color),
                     c.type == T_NUM ? "" : " ");
            break;
    }
    cur = nxt;
}

// Winner screen
static void show_winner(const char* name) {
    ncplane_erase(std);
    unsigned dimy, dimx;
    ncplane_dim_yx(std, &dimy, &dimx);
    const char* art[] = {
        " ██████╗  █████╗ ███╗   ███╗███████╗     ██████╗ ██╗   ██╗███████╗██████╗ ██╗",
        "██╔════╝ ██╔══██╗████╗ ████║██╔════╝    ██╔═══██╗██║   ██║██╔════╝██╔══██╗██║",
        "██║  ███╗███████║██╔████╔██║█████╗      ██║   ██║██║   ██║█████╗  ██████╔╝██║",
        "██║   ██║██╔══██║██║╚██╔╝██║██╔══╝      ██║   ██║╚██╗ ██╔╝██╔══╝  ██╔══██╗╚═╝",
        "╚██████╔╝██║  ██║██║ ╚═╝ ██║███████╗    ╚██████╔╝ ╚████╔╝ ███████╗██║  ██║██╗",
        " ╚═════╝ ╚═╝  ╚═╝╚═╝     ╚═╝╚══════╝     ╚═════╝   ╚═══╝  ╚══════╝╚═╝  ╚═╝╚═╝",
    };
    ncplane_set_fg_rgb8(std, 255, 215, 0);
    for (int i = 0; i < 6; i++)
        ncplane_putstr_yx(std, (int)dimy/2 - 6 + i, (int)dimx/2 - 38, art[i]);
    char buf[64];
    snprintf(buf, sizeof buf, "%s win(s) the game!", name);
    ncplane_set_fg_rgb8(std, 255, 255, 255);
    ncplane_putstr_yx(std, (int)dimy/2 + 1, (int)dimx/2 - (int)strlen(buf)/2, buf);
    notcurses_render(nc);
    usleep(4000000); // brief pause
}

// Main func
int main(void) {
    srand((unsigned)time(NULL));

    struct notcurses_options opts;
    memset(&opts, 0, sizeof opts);
    opts.flags = NCOPTION_SUPPRESS_BANNERS;
    nc = notcurses_core_init(&opts, NULL);
    if (!nc) {
        fprintf(stderr, "Failed to initialize notcurses (need a real terminal).\n");
        return 1;
    }
    std = notcurses_stdplane(nc);
    notcurses_cursor_disable(nc);

    // Setup players
    strcpy(players[0].name, "You");   players[0].is_human = 1;
    strcpy(players[1].name, "P2");    players[1].is_human = 0;
    strcpy(players[2].name, "P3");    players[2].is_human = 0;
    strcpy(players[3].name, "P4");    players[3].is_human = 0;
    for (int i = 0; i < NPLAYERS; i++) players[i].n = 0;

    build_deck();
    shuffle_deck();

    for (int r = 0; r < 7; r++)
        for (int i = 0; i < NPLAYERS; i++)
            give_card(&players[i], draw_card());

    // first discard card: keep drawing until it's a plain number (no starting with wilds)
    Card first;
    do { first = draw_card(); } while (first.type == T_WILD || first.type == T_WILD4);
    discard[discard_n++] = first;
    active_color = first.color;
    if (first.type == T_REVERSE) dir = -dir;
    if (first.type == T_SKIP) cur = next_player(cur);
    if (first.type == T_DRAW2) {
        give_card(&players[cur], draw_card());
        give_card(&players[cur], draw_card());
        cur = next_player(cur);
    }

    set_status("Game start! Your turn." );

    int sel = 0;
    int running = 1;

    while (running) {
        // check for game over
        for (int i = 0; i < NPLAYERS; i++) {
            if (players[i].n == 0) {
                render(sel);
                show_winner(players[i].name);
                running = 0;
                break;
            }
        }
        if (!running) break;

        Player* p = &players[cur];
        Card top = discard[discard_n - 1];

        if (p->is_human) {
            if (sel >= p->n) sel = p->n - 1;
            if (sel < 0) sel = 0;
            render(sel);

            ncinput ni;
            uint32_t id = notcurses_get_blocking(nc, &ni);
            if (ni.evtype == NCTYPE_RELEASE) continue;

            if (id == 'q' || id == 'Q') { running = 0; }
            else if (id == NCKEY_LEFT || id == 'h') { if (p->n) sel = (sel + p->n - 1) % p->n; }
            else if (id == NCKEY_RIGHT || id == 'l') { if (p->n) sel = (sel + 1) % p->n; }
            else if (id == 'd' || id == 'D') {
                Card c = draw_card();
                give_card(p, c);
                if (card_playable(&c, active_color, &top)) {
                    snprintf(msg, sizeof msg, "You drew a playable card: %s.",
                             c.type == T_NUM ? color_name(c.color) : type_label(c.type));
                } else {
                    snprintf(msg, sizeof msg, "You drew a card. Turn passes.");
                    cur = next_player(cur);
                }
            }
            else if (id == NCKEY_ENTER || id == ' ') {
                if (p->n == 0) continue;
                Card c = p->hand[sel];
                if (!card_playable(&c, active_color, &top)) {
                    snprintf(msg, sizeof msg, "That card doesn't match — pick another or press d.");
                } else {
                    Color chosen = C_NONE;
                    if (c.type == T_WILD || c.type == T_WILD4) {
                        render(sel);
                        chosen = human_pick_color();
                    }
                    play_card(p, sel, chosen);
                    if (sel >= p->n && sel > 0) sel--;
                }
            }
        } else {
            // bot turn
            render(sel);
            usleep(2000000); // brief pause
            int idx = ai_pick_card(p, active_color, &top);
            if (idx == -1) {
                Card c = draw_card();
                give_card(p, c);
                if (card_playable(&c, active_color, &top)) {
                    idx = p->n - 1;
                } else {
                    snprintf(msg, sizeof msg, "%s drew a card.", p->name);
                    cur = next_player(cur);
                    continue;
                }
            }
            Color chosen = C_NONE;
            if (p->hand[idx].type == T_WILD || p->hand[idx].type == T_WILD4)
                chosen = ai_choose_color(p);
            play_card(p, idx, chosen);
        }
    }

    notcurses_stop(nc);
    return 0;
}
