// SPDX-License-Identifier: GPL-2.0-only
// Generated from official DOOM struct field order by tools/generate_save_codec.py.
// Modified 2026-09-30: byte-compatible 32-bit disk layouts without native addresses.
#pragma pack(push, 4)
typedef struct {
    uint32_t prev;
    uint32_t next;
    uint32_t function;
} disk_thinker_t;
_Static_assert(sizeof(disk_thinker_t) == 12, "thinker_t original ILP32 layout");
typedef struct {
    uint32_t state;
    int tics;
    fixed_t sx;
    fixed_t sy;
} disk_pspdef_t;
_Static_assert(sizeof(disk_pspdef_t) == 16, "pspdef_t original ILP32 layout");
typedef struct {
    uint32_t mo;
    playerstate_t playerstate;
    ticcmd_t cmd;
    fixed_t viewz;
    fixed_t viewheight;
    fixed_t deltaviewheight;
    fixed_t bob;
    int health;
    int armorpoints;
    int armortype;
    int powers[NUMPOWERS];
    boolean cards[NUMCARDS];
    boolean backpack;
    int frags[MAXPLAYERS];
    weapontype_t readyweapon;
    weapontype_t pendingweapon;
    boolean weaponowned[NUMWEAPONS];
    int ammo[NUMAMMO];
    int maxammo[NUMAMMO];
    int attackdown;
    int usedown;
    int cheats;
    int refire;
    int killcount;
    int itemcount;
    int secretcount;
    uint32_t message;
    int damagecount;
    int bonuscount;
    uint32_t attacker;
    int extralight;
    int fixedcolormap;
    int colormap;
    disk_pspdef_t psprites[NUMPSPRITES];
    boolean didsecret;
} disk_player_t;
_Static_assert(sizeof(disk_player_t) == 280, "player_t original ILP32 layout");
typedef struct {
    disk_thinker_t thinker;
    fixed_t x;
    fixed_t y;
    fixed_t z;
    uint32_t snext;
    uint32_t sprev;
    angle_t angle;
    spritenum_t sprite;
    int frame;
    uint32_t bnext;
    uint32_t bprev;
    uint32_t subsector;
    fixed_t floorz;
    fixed_t ceilingz;
    fixed_t radius;
    fixed_t height;
    fixed_t momx;
    fixed_t momy;
    fixed_t momz;
    int validcount;
    mobjtype_t type;
    uint32_t info;
    int tics;
    uint32_t state;
    int flags;
    int health;
    int movedir;
    int movecount;
    uint32_t target;
    int reactiontime;
    int threshold;
    uint32_t player;
    int lastlook;
    mapthing_t spawnpoint;
    uint32_t tracer;
} disk_mobj_t;
_Static_assert(sizeof(disk_mobj_t) == 156, "mobj_t original ILP32 layout");
typedef struct {
    disk_thinker_t thinker;
    ceiling_e type;
    uint32_t sector;
    fixed_t bottomheight;
    fixed_t topheight;
    fixed_t speed;
    boolean crush;
    int direction;
    int tag;
    int olddirection;
} disk_ceiling_t;
_Static_assert(sizeof(disk_ceiling_t) == 48, "ceiling_t original ILP32 layout");
typedef struct {
    disk_thinker_t thinker;
    vldoor_e type;
    uint32_t sector;
    fixed_t topheight;
    fixed_t speed;
    int direction;
    int topwait;
    int topcountdown;
} disk_vldoor_t;
_Static_assert(sizeof(disk_vldoor_t) == 40, "vldoor_t original ILP32 layout");
typedef struct {
    disk_thinker_t thinker;
    floor_e type;
    boolean crush;
    uint32_t sector;
    int direction;
    int newspecial;
    short texture;
    fixed_t floordestheight;
    fixed_t speed;
} disk_floormove_t;
_Static_assert(sizeof(disk_floormove_t) == 44, "floormove_t original ILP32 layout");
typedef struct {
    disk_thinker_t thinker;
    uint32_t sector;
    fixed_t speed;
    fixed_t low;
    fixed_t high;
    int wait;
    int count;
    plat_e status;
    plat_e oldstatus;
    boolean crush;
    int tag;
    plattype_e type;
} disk_plat_t;
_Static_assert(sizeof(disk_plat_t) == 56, "plat_t original ILP32 layout");
typedef struct {
    disk_thinker_t thinker;
    uint32_t sector;
    int count;
    int maxlight;
    int minlight;
    int maxtime;
    int mintime;
} disk_lightflash_t;
_Static_assert(sizeof(disk_lightflash_t) == 36, "lightflash_t original ILP32 layout");
typedef struct {
    disk_thinker_t thinker;
    uint32_t sector;
    int count;
    int minlight;
    int maxlight;
    int darktime;
    int brighttime;
} disk_strobe_t;
_Static_assert(sizeof(disk_strobe_t) == 36, "strobe_t original ILP32 layout");
typedef struct {
    disk_thinker_t thinker;
    uint32_t sector;
    int minlight;
    int maxlight;
    int direction;
} disk_glow_t;
_Static_assert(sizeof(disk_glow_t) == 28, "glow_t original ILP32 layout");
#pragma pack(pop)
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "This save codec targets little-endian arm64 macOS"
#endif
static disk_thinker_t Encode_thinker_t(const thinker_t *n) {
    disk_thinker_t d = {0};
    d.prev = 0;
    d.next = 0;
    d.function = n->function.acp1 != NULL;
    return d;
}
static void Decode_thinker_t(thinker_t *n, const disk_thinker_t *d) {
    memset(n, 0, sizeof(*n));
    n->prev = (struct thinker_s*)(uintptr_t)d->prev;
    n->next = (struct thinker_s*)(uintptr_t)d->next;
    n->function.acp1 = d->function ? (actionf_p1)(uintptr_t)1 : NULL;
}
static void SaveWrite_thinker_t(const thinker_t *n) {
    disk_thinker_t d = Encode_thinker_t(n);
    P_SaveCheck(sizeof(d)); memcpy(save_p, &d, sizeof(d)); save_p += sizeof(d);
}
static void SaveRead_thinker_t(thinker_t *n) {
    disk_thinker_t d;
    P_SaveCheck(sizeof(d)); memcpy(&d, save_p, sizeof(d)); save_p += sizeof(d);
    Decode_thinker_t(n, &d);
}
static disk_pspdef_t Encode_pspdef_t(const pspdef_t *n) {
    disk_pspdef_t d = {0};
    d.state = (uint32_t)(uintptr_t)n->state;
    d.tics = n->tics;
    d.sx = n->sx;
    d.sy = n->sy;
    return d;
}
static void Decode_pspdef_t(pspdef_t *n, const disk_pspdef_t *d) {
    memset(n, 0, sizeof(*n));
    n->state = (state_t*)(uintptr_t)d->state;
    n->tics = d->tics;
    n->sx = d->sx;
    n->sy = d->sy;
}
static void SaveWrite_pspdef_t(const pspdef_t *n) {
    disk_pspdef_t d = Encode_pspdef_t(n);
    P_SaveCheck(sizeof(d)); memcpy(save_p, &d, sizeof(d)); save_p += sizeof(d);
}
static void SaveRead_pspdef_t(pspdef_t *n) {
    disk_pspdef_t d;
    P_SaveCheck(sizeof(d)); memcpy(&d, save_p, sizeof(d)); save_p += sizeof(d);
    Decode_pspdef_t(n, &d);
}
static disk_player_t Encode_player_t(const player_t *n) {
    disk_player_t d = {0};
    d.mo = 0;
    d.playerstate = n->playerstate;
    d.cmd = n->cmd;
    d.viewz = n->viewz;
    d.viewheight = n->viewheight;
    d.deltaviewheight = n->deltaviewheight;
    d.bob = n->bob;
    d.health = n->health;
    d.armorpoints = n->armorpoints;
    d.armortype = n->armortype;
    memcpy(d.powers, n->powers, sizeof(d.powers));
    memcpy(d.cards, n->cards, sizeof(d.cards));
    d.backpack = n->backpack;
    memcpy(d.frags, n->frags, sizeof(d.frags));
    d.readyweapon = n->readyweapon;
    d.pendingweapon = n->pendingweapon;
    memcpy(d.weaponowned, n->weaponowned, sizeof(d.weaponowned));
    memcpy(d.ammo, n->ammo, sizeof(d.ammo));
    memcpy(d.maxammo, n->maxammo, sizeof(d.maxammo));
    d.attackdown = n->attackdown;
    d.usedown = n->usedown;
    d.cheats = n->cheats;
    d.refire = n->refire;
    d.killcount = n->killcount;
    d.itemcount = n->itemcount;
    d.secretcount = n->secretcount;
    d.message = 0;
    d.damagecount = n->damagecount;
    d.bonuscount = n->bonuscount;
    d.attacker = 0;
    d.extralight = n->extralight;
    d.fixedcolormap = n->fixedcolormap;
    d.colormap = n->colormap;
    for (int i=0; i<NUMPSPRITES; ++i) d.psprites[i] = Encode_pspdef_t(&n->psprites[i]);
    d.didsecret = n->didsecret;
    return d;
}
static void Decode_player_t(player_t *n, const disk_player_t *d) {
    memset(n, 0, sizeof(*n));
    n->mo = (mobj_t*)(uintptr_t)d->mo;
    n->playerstate = d->playerstate;
    n->cmd = d->cmd;
    n->viewz = d->viewz;
    n->viewheight = d->viewheight;
    n->deltaviewheight = d->deltaviewheight;
    n->bob = d->bob;
    n->health = d->health;
    n->armorpoints = d->armorpoints;
    n->armortype = d->armortype;
    memcpy(n->powers, d->powers, sizeof(d->powers));
    memcpy(n->cards, d->cards, sizeof(d->cards));
    n->backpack = d->backpack;
    memcpy(n->frags, d->frags, sizeof(d->frags));
    n->readyweapon = d->readyweapon;
    n->pendingweapon = d->pendingweapon;
    memcpy(n->weaponowned, d->weaponowned, sizeof(d->weaponowned));
    memcpy(n->ammo, d->ammo, sizeof(d->ammo));
    memcpy(n->maxammo, d->maxammo, sizeof(d->maxammo));
    n->attackdown = d->attackdown;
    n->usedown = d->usedown;
    n->cheats = d->cheats;
    n->refire = d->refire;
    n->killcount = d->killcount;
    n->itemcount = d->itemcount;
    n->secretcount = d->secretcount;
    n->message = (char*)(uintptr_t)d->message;
    n->damagecount = d->damagecount;
    n->bonuscount = d->bonuscount;
    n->attacker = (mobj_t*)(uintptr_t)d->attacker;
    n->extralight = d->extralight;
    n->fixedcolormap = d->fixedcolormap;
    n->colormap = d->colormap;
    for (int i=0; i<NUMPSPRITES; ++i) Decode_pspdef_t(&n->psprites[i], &d->psprites[i]);
    n->didsecret = d->didsecret;
}
static void SaveWrite_player_t(const player_t *n) {
    disk_player_t d = Encode_player_t(n);
    P_SaveCheck(sizeof(d)); memcpy(save_p, &d, sizeof(d)); save_p += sizeof(d);
}
static void SaveRead_player_t(player_t *n) {
    disk_player_t d;
    P_SaveCheck(sizeof(d)); memcpy(&d, save_p, sizeof(d)); save_p += sizeof(d);
    Decode_player_t(n, &d);
}
static disk_mobj_t Encode_mobj_t(const mobj_t *n) {
    disk_mobj_t d = {0};
    d.thinker = Encode_thinker_t(&n->thinker);
    d.x = n->x;
    d.y = n->y;
    d.z = n->z;
    d.snext = 0;
    d.sprev = 0;
    d.angle = n->angle;
    d.sprite = n->sprite;
    d.frame = n->frame;
    d.bnext = 0;
    d.bprev = 0;
    d.subsector = 0;
    d.floorz = n->floorz;
    d.ceilingz = n->ceilingz;
    d.radius = n->radius;
    d.height = n->height;
    d.momx = n->momx;
    d.momy = n->momy;
    d.momz = n->momz;
    d.validcount = n->validcount;
    d.type = n->type;
    d.info = 0;
    d.tics = n->tics;
    d.state = (uint32_t)(uintptr_t)n->state;
    d.flags = n->flags;
    d.health = n->health;
    d.movedir = n->movedir;
    d.movecount = n->movecount;
    d.target = 0;
    d.reactiontime = n->reactiontime;
    d.threshold = n->threshold;
    d.player = (uint32_t)(uintptr_t)n->player;
    d.lastlook = n->lastlook;
    d.spawnpoint = n->spawnpoint;
    d.tracer = 0;
    return d;
}
static void Decode_mobj_t(mobj_t *n, const disk_mobj_t *d) {
    memset(n, 0, sizeof(*n));
    Decode_thinker_t(&n->thinker, &d->thinker);
    n->x = d->x;
    n->y = d->y;
    n->z = d->z;
    n->snext = (struct mobj_s*)(uintptr_t)d->snext;
    n->sprev = (struct mobj_s*)(uintptr_t)d->sprev;
    n->angle = d->angle;
    n->sprite = d->sprite;
    n->frame = d->frame;
    n->bnext = (struct mobj_s*)(uintptr_t)d->bnext;
    n->bprev = (struct mobj_s*)(uintptr_t)d->bprev;
    n->subsector = (struct subsector_s*)(uintptr_t)d->subsector;
    n->floorz = d->floorz;
    n->ceilingz = d->ceilingz;
    n->radius = d->radius;
    n->height = d->height;
    n->momx = d->momx;
    n->momy = d->momy;
    n->momz = d->momz;
    n->validcount = d->validcount;
    n->type = d->type;
    n->info = (mobjinfo_t*)(uintptr_t)d->info;
    n->tics = d->tics;
    n->state = (state_t*)(uintptr_t)d->state;
    n->flags = d->flags;
    n->health = d->health;
    n->movedir = d->movedir;
    n->movecount = d->movecount;
    n->target = (struct mobj_s*)(uintptr_t)d->target;
    n->reactiontime = d->reactiontime;
    n->threshold = d->threshold;
    n->player = (struct player_s*)(uintptr_t)d->player;
    n->lastlook = d->lastlook;
    n->spawnpoint = d->spawnpoint;
    n->tracer = (struct mobj_s*)(uintptr_t)d->tracer;
}
static void SaveWrite_mobj_t(const mobj_t *n) {
    disk_mobj_t d = Encode_mobj_t(n);
    P_SaveCheck(sizeof(d)); memcpy(save_p, &d, sizeof(d)); save_p += sizeof(d);
}
static void SaveRead_mobj_t(mobj_t *n) {
    disk_mobj_t d;
    P_SaveCheck(sizeof(d)); memcpy(&d, save_p, sizeof(d)); save_p += sizeof(d);
    Decode_mobj_t(n, &d);
}
static disk_ceiling_t Encode_ceiling_t(const ceiling_t *n) {
    disk_ceiling_t d = {0};
    d.thinker = Encode_thinker_t(&n->thinker);
    d.type = n->type;
    d.sector = (uint32_t)(uintptr_t)n->sector;
    d.bottomheight = n->bottomheight;
    d.topheight = n->topheight;
    d.speed = n->speed;
    d.crush = n->crush;
    d.direction = n->direction;
    d.tag = n->tag;
    d.olddirection = n->olddirection;
    return d;
}
static void Decode_ceiling_t(ceiling_t *n, const disk_ceiling_t *d) {
    memset(n, 0, sizeof(*n));
    Decode_thinker_t(&n->thinker, &d->thinker);
    n->type = d->type;
    n->sector = (sector_t*)(uintptr_t)d->sector;
    n->bottomheight = d->bottomheight;
    n->topheight = d->topheight;
    n->speed = d->speed;
    n->crush = d->crush;
    n->direction = d->direction;
    n->tag = d->tag;
    n->olddirection = d->olddirection;
}
static void SaveWrite_ceiling_t(const ceiling_t *n) {
    disk_ceiling_t d = Encode_ceiling_t(n);
    P_SaveCheck(sizeof(d)); memcpy(save_p, &d, sizeof(d)); save_p += sizeof(d);
}
static void SaveRead_ceiling_t(ceiling_t *n) {
    disk_ceiling_t d;
    P_SaveCheck(sizeof(d)); memcpy(&d, save_p, sizeof(d)); save_p += sizeof(d);
    Decode_ceiling_t(n, &d);
}
static disk_vldoor_t Encode_vldoor_t(const vldoor_t *n) {
    disk_vldoor_t d = {0};
    d.thinker = Encode_thinker_t(&n->thinker);
    d.type = n->type;
    d.sector = (uint32_t)(uintptr_t)n->sector;
    d.topheight = n->topheight;
    d.speed = n->speed;
    d.direction = n->direction;
    d.topwait = n->topwait;
    d.topcountdown = n->topcountdown;
    return d;
}
static void Decode_vldoor_t(vldoor_t *n, const disk_vldoor_t *d) {
    memset(n, 0, sizeof(*n));
    Decode_thinker_t(&n->thinker, &d->thinker);
    n->type = d->type;
    n->sector = (sector_t*)(uintptr_t)d->sector;
    n->topheight = d->topheight;
    n->speed = d->speed;
    n->direction = d->direction;
    n->topwait = d->topwait;
    n->topcountdown = d->topcountdown;
}
static void SaveWrite_vldoor_t(const vldoor_t *n) {
    disk_vldoor_t d = Encode_vldoor_t(n);
    P_SaveCheck(sizeof(d)); memcpy(save_p, &d, sizeof(d)); save_p += sizeof(d);
}
static void SaveRead_vldoor_t(vldoor_t *n) {
    disk_vldoor_t d;
    P_SaveCheck(sizeof(d)); memcpy(&d, save_p, sizeof(d)); save_p += sizeof(d);
    Decode_vldoor_t(n, &d);
}
static disk_floormove_t Encode_floormove_t(const floormove_t *n) {
    disk_floormove_t d = {0};
    d.thinker = Encode_thinker_t(&n->thinker);
    d.type = n->type;
    d.crush = n->crush;
    d.sector = (uint32_t)(uintptr_t)n->sector;
    d.direction = n->direction;
    d.newspecial = n->newspecial;
    d.texture = n->texture;
    d.floordestheight = n->floordestheight;
    d.speed = n->speed;
    return d;
}
static void Decode_floormove_t(floormove_t *n, const disk_floormove_t *d) {
    memset(n, 0, sizeof(*n));
    Decode_thinker_t(&n->thinker, &d->thinker);
    n->type = d->type;
    n->crush = d->crush;
    n->sector = (sector_t*)(uintptr_t)d->sector;
    n->direction = d->direction;
    n->newspecial = d->newspecial;
    n->texture = d->texture;
    n->floordestheight = d->floordestheight;
    n->speed = d->speed;
}
static void SaveWrite_floormove_t(const floormove_t *n) {
    disk_floormove_t d = Encode_floormove_t(n);
    P_SaveCheck(sizeof(d)); memcpy(save_p, &d, sizeof(d)); save_p += sizeof(d);
}
static void SaveRead_floormove_t(floormove_t *n) {
    disk_floormove_t d;
    P_SaveCheck(sizeof(d)); memcpy(&d, save_p, sizeof(d)); save_p += sizeof(d);
    Decode_floormove_t(n, &d);
}
static disk_plat_t Encode_plat_t(const plat_t *n) {
    disk_plat_t d = {0};
    d.thinker = Encode_thinker_t(&n->thinker);
    d.sector = (uint32_t)(uintptr_t)n->sector;
    d.speed = n->speed;
    d.low = n->low;
    d.high = n->high;
    d.wait = n->wait;
    d.count = n->count;
    d.status = n->status;
    d.oldstatus = n->oldstatus;
    d.crush = n->crush;
    d.tag = n->tag;
    d.type = n->type;
    return d;
}
static void Decode_plat_t(plat_t *n, const disk_plat_t *d) {
    memset(n, 0, sizeof(*n));
    Decode_thinker_t(&n->thinker, &d->thinker);
    n->sector = (sector_t*)(uintptr_t)d->sector;
    n->speed = d->speed;
    n->low = d->low;
    n->high = d->high;
    n->wait = d->wait;
    n->count = d->count;
    n->status = d->status;
    n->oldstatus = d->oldstatus;
    n->crush = d->crush;
    n->tag = d->tag;
    n->type = d->type;
}
static void SaveWrite_plat_t(const plat_t *n) {
    disk_plat_t d = Encode_plat_t(n);
    P_SaveCheck(sizeof(d)); memcpy(save_p, &d, sizeof(d)); save_p += sizeof(d);
}
static void SaveRead_plat_t(plat_t *n) {
    disk_plat_t d;
    P_SaveCheck(sizeof(d)); memcpy(&d, save_p, sizeof(d)); save_p += sizeof(d);
    Decode_plat_t(n, &d);
}
static disk_lightflash_t Encode_lightflash_t(const lightflash_t *n) {
    disk_lightflash_t d = {0};
    d.thinker = Encode_thinker_t(&n->thinker);
    d.sector = (uint32_t)(uintptr_t)n->sector;
    d.count = n->count;
    d.maxlight = n->maxlight;
    d.minlight = n->minlight;
    d.maxtime = n->maxtime;
    d.mintime = n->mintime;
    return d;
}
static void Decode_lightflash_t(lightflash_t *n, const disk_lightflash_t *d) {
    memset(n, 0, sizeof(*n));
    Decode_thinker_t(&n->thinker, &d->thinker);
    n->sector = (sector_t*)(uintptr_t)d->sector;
    n->count = d->count;
    n->maxlight = d->maxlight;
    n->minlight = d->minlight;
    n->maxtime = d->maxtime;
    n->mintime = d->mintime;
}
static void SaveWrite_lightflash_t(const lightflash_t *n) {
    disk_lightflash_t d = Encode_lightflash_t(n);
    P_SaveCheck(sizeof(d)); memcpy(save_p, &d, sizeof(d)); save_p += sizeof(d);
}
static void SaveRead_lightflash_t(lightflash_t *n) {
    disk_lightflash_t d;
    P_SaveCheck(sizeof(d)); memcpy(&d, save_p, sizeof(d)); save_p += sizeof(d);
    Decode_lightflash_t(n, &d);
}
static disk_strobe_t Encode_strobe_t(const strobe_t *n) {
    disk_strobe_t d = {0};
    d.thinker = Encode_thinker_t(&n->thinker);
    d.sector = (uint32_t)(uintptr_t)n->sector;
    d.count = n->count;
    d.minlight = n->minlight;
    d.maxlight = n->maxlight;
    d.darktime = n->darktime;
    d.brighttime = n->brighttime;
    return d;
}
static void Decode_strobe_t(strobe_t *n, const disk_strobe_t *d) {
    memset(n, 0, sizeof(*n));
    Decode_thinker_t(&n->thinker, &d->thinker);
    n->sector = (sector_t*)(uintptr_t)d->sector;
    n->count = d->count;
    n->minlight = d->minlight;
    n->maxlight = d->maxlight;
    n->darktime = d->darktime;
    n->brighttime = d->brighttime;
}
static void SaveWrite_strobe_t(const strobe_t *n) {
    disk_strobe_t d = Encode_strobe_t(n);
    P_SaveCheck(sizeof(d)); memcpy(save_p, &d, sizeof(d)); save_p += sizeof(d);
}
static void SaveRead_strobe_t(strobe_t *n) {
    disk_strobe_t d;
    P_SaveCheck(sizeof(d)); memcpy(&d, save_p, sizeof(d)); save_p += sizeof(d);
    Decode_strobe_t(n, &d);
}
static disk_glow_t Encode_glow_t(const glow_t *n) {
    disk_glow_t d = {0};
    d.thinker = Encode_thinker_t(&n->thinker);
    d.sector = (uint32_t)(uintptr_t)n->sector;
    d.minlight = n->minlight;
    d.maxlight = n->maxlight;
    d.direction = n->direction;
    return d;
}
static void Decode_glow_t(glow_t *n, const disk_glow_t *d) {
    memset(n, 0, sizeof(*n));
    Decode_thinker_t(&n->thinker, &d->thinker);
    n->sector = (sector_t*)(uintptr_t)d->sector;
    n->minlight = d->minlight;
    n->maxlight = d->maxlight;
    n->direction = d->direction;
}
static void SaveWrite_glow_t(const glow_t *n) {
    disk_glow_t d = Encode_glow_t(n);
    P_SaveCheck(sizeof(d)); memcpy(save_p, &d, sizeof(d)); save_p += sizeof(d);
}
static void SaveRead_glow_t(glow_t *n) {
    disk_glow_t d;
    P_SaveCheck(sizeof(d)); memcpy(&d, save_p, sizeof(d)); save_p += sizeof(d);
    Decode_glow_t(n, &d);
}
