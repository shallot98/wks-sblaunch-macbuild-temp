/*
 * WeTypeVoiceSBLaunch 0.5.0
 *
 * C-only: Logos/Foundation constructors have put SpringBoard into .eksafemode.
 *
 * The keyboard extension blocks the official voice openURL, so SpringBoard
 * itself opens com.tencent.wetype *suspended* through the workspace path — the
 * only layer that honours processLaunchIntent / __SBWorkspaceOpenOptionUnlockResult.
 * While the voice token is held the WeType scene is pinned backgrounded at the
 * central settings commit point. No token means no touching, so tapping the
 * WeType icon still foregrounds normally.
 */
#include <objc/runtime.h>
#include <objc/message.h>
#include <dispatch/dispatch.h>
#include <notify.h>
#include <substrate.h>
#include <dlfcn.h>
#include <pthread.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <os/log.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

extern void *_Block_copy(const void *);

static void (*OrigFBOpen)(id, SEL, id, id, id, id, id);
static BOOL (*OrigAllow)(id, SEL, id);
static void (*OrigFBSOpenURL)(id, SEL, id, id, id, unsigned int, id);
static void (*OrigFBSOpenApp)(id, SEL, id, id, unsigned int, id);
static void (*OrigMgrApply)(id, SEL, id, id, id, id);
static void (*OrigMgrNoteFG)(id, SEL, id);
static void (*OrigSceneBlock)(id, SEL, id);
static void (*OrigSceneUp3)(id, SEL, id, id, id);
static void (*OrigSceneUp2)(id, SEL, id, id);
static void (*OrigScenePerf)(id, SEL, id);
static void (*OrigScenePerf2)(id, SEL, id, id);

static const char *kWeTypeBundle = "com.tencent.wetype";
static const char *kSpringBoardBundle = "com.apple.springboard";
static const char *kVoiceURL = "wetype://WXKBURL_STARTVOICERECORD";
static const char *kNotifyLaunch = "com.wxkb.wetypehost.launch";
static const char *kNotifyReady = "com.wxkb.wetypehost.ready";
static const char *kPendingFile = "/var/mobile/Library/Preferences/wks_pending_voice.url";
static char gVoiceHost[128];
static const char *kOffFile = "/var/mobile/Library/Preferences/com.wxkb.sblaunch.off";
static const char *kBootsFile = "/var/mobile/Library/Preferences/wks_sblaunch.boots";

static double gVoiceUntil;   /* legacy: mutate an official request that still arrives */
static double gTokUntil;     /* voice-launch token: this suspended open is ours */
static double gQuietUntil;   /* debounce so our own retire does not loop */
static double gLastKick;
static int gTokRetired;
static int gActivated;      /* this wxkb instance already got a real activation */
static int gInstalled;
static int gApplyHooked;
static int gApplyReported;
static int gSceneReported;
static int gProbed;
static int gNotifyRegistered;

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void wlog(const char *fmt, ...) {
    char buf[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    os_log(OS_LOG_DEFAULT, "WKS SB %{public}s", buf);
    FILE *f = fopen("/var/mobile/Library/Preferences/wks_sblaunch.log", "a");
    if (!f) f = fopen("/var/mobile/Library/Logs/wks_sblaunch.log", "a");
    if (!f) f = fopen("/var/tmp/wks_sblaunch.log", "a");
    if (f) {
        fprintf(f, "%.0f %s\n", now_s(), buf);
        fclose(f);
    }
}

/* Every notable event is also announced as a Darwin notify, because a confined
 * process cannot write the shared preference paths and a missing log file would
 * look identical to "not injected". */
static void hb(const char *what) {
    char name[96];
    snprintf(name, sizeof(name), "com.wxkb.sblaunch.%s", what);
    notify_post(name);
}

static int disabled(void) {
    struct stat st;
    return stat(kOffFile, &st) == 0;
}

static int in_voice(void) {
    return now_s() < gVoiceUntil;
}

static void mark_voice(const char *why) {
    gVoiceUntil = now_s() + 20.0;
    wlog("voice-window 20s (%s)", why);
}

/* ---------- voice-launch token ---------- */

static pid_t wxkb_pid(void);

static int token_valid(void) {
    if (gTokUntil <= 0.0) return 0;
    if (now_s() >= gTokUntil) {
        wlog("token expired");
        gTokUntil = 0.0;
        gTokRetired = 0;
        return 0;
    }
    return 1;
}

static void token_grant(const char *why) {
    gTokUntil = now_s() + 20.0;
    gTokRetired = 0;
    if (wxkb_pid() <= 0) gActivated = 0;
    hb("token");
    wlog("token grant 20s (%s)", why);
}

static void token_clear(const char *why) {
    if (gTokUntil > 0.0) wlog("token clear (%s)", why);
    gTokUntil = 0.0;
    gTokRetired = 0;
}

/* ---------- objc helpers (pure C) ---------- */

static int resp(id obj, const char *selname) {
    if (!obj) return 0;
    return ((BOOL (*)(id, SEL, SEL))objc_msgSend)(
        obj, sel_registerName("respondsToSelector:"), sel_registerName(selname)) ? 1 : 0;
}

static int cresp(Class cls, const char *selname) {
    if (!cls) return 0;
    return ((BOOL (*)(id, SEL, SEL))objc_msgSend)(
        (id)cls, sel_registerName("respondsToSelector:"), sel_registerName(selname)) ? 1 : 0;
}

static id msg0(id obj, const char *selname) {
    if (!obj || !resp(obj, selname)) return nil;
    return ((id (*)(id, SEL))objc_msgSend)(obj, sel_registerName(selname));
}

static id cls0(Class cls, const char *selname) {
    if (!cresp(cls, selname)) return nil;
    return ((id (*)(id, SEL))objc_msgSend)((id)cls, sel_registerName(selname));
}

static id msg1(id obj, const char *selname, id a1) {
    if (!obj || !resp(obj, selname)) return nil;
    return ((id (*)(id, SEL, id))objc_msgSend)(obj, sel_registerName(selname), a1);
}

static void call1(id obj, const char *selname, id a1) {
    if (!obj || !resp(obj, selname)) return;
    ((void (*)(id, SEL, id))objc_msgSend)(obj, sel_registerName(selname), a1);
}

static const char *cstr(id obj) {
    if (!obj) return "";
    if (!resp(obj, "UTF8String")) {
        obj = msg0(obj, "description");
        if (!obj) return "";
    }
    if (!resp(obj, "UTF8String")) return "";
    const char *p = ((const char *(*)(id, SEL))objc_msgSend)(obj, sel_registerName("UTF8String"));
    return p ? p : "";
}

static id nsstr(const char *s) {
    Class C = objc_getClass("NSString");
    if (!C || !s) return nil;
    return ((id (*)(id, SEL, const char *))objc_msgSend)(
        (id)C, sel_registerName("stringWithUTF8String:"), s);
}

static id nsnewdict(void) {
    Class MD = objc_getClass("NSMutableDictionary");
    if (!MD) return nil;
    return ((id (*)(id, SEL))objc_msgSend)((id)MD, sel_registerName("dictionary"));
}

static id nsnum_yes(void) {
    Class C = objc_getClass("NSNumber");
    if (!C) return nil;
    return ((id (*)(id, SEL, BOOL))objc_msgSend)(
        (id)C, sel_registerName("numberWithBool:"), YES);
}

static id nsnum_int(int v) {
    Class C = objc_getClass("NSNumber");
    if (!C) return nil;
    return ((id (*)(id, SEL, int))objc_msgSend)(
        (id)C, sel_registerName("numberWithInt:"), v);
}

static id dict_get(id dict, const char *key) {
    if (!dict || !resp(dict, "objectForKey:")) return nil;
    return ((id (*)(id, SEL, id))objc_msgSend)(
        dict, sel_registerName("objectForKey:"), nsstr(key));
}

static void dict_set(id dict, const char *key, id val) {
    if (!dict || !val || !resp(dict, "setObject:forKey:")) return;
    ((void (*)(id, SEL, id, id))objc_msgSend)(
        dict, sel_registerName("setObject:forKey:"), val, nsstr(key));
}

static int cls_has(const char *clsname, const char *selname) {
    Class c = objc_getClass(clsname);
    return (c && class_getInstanceMethod(c, sel_registerName(selname))) ? 1 : 0;
}

/* ---------- identity predicates ---------- */

static int is_wetype_str(const char *p) {
    if (!p || !p[0]) return 0;
    if (strstr(p, "wetype.keyboard") || strstr(p, "wxkb_plugin")) return 0;
    return strstr(p, "tencent.wetype") ? 1 : 0;
}

static int is_voice(id obj) {
    const char *p = cstr(obj);
    if (!p[0]) return 0;
    if (strcasestr(p, "startvoicerecord")) return 1;
    if (strcasestr(p, "wetype:") && (strcasestr(p, "sid=") || strcasestr(p, "lbt=")))
        return 1;
    return 0;
}

static int is_wetype(id obj) {
    return is_wetype_str(cstr(obj));
}

static int opts_is_voice(id opts) {
    if (is_voice(opts)) return 1;
    id dict = resp(opts, "dictionary") ? msg0(opts, "dictionary") : opts;
    if (!dict) return 0;
    const char *keys[] = {
        "__PayloadURL", "UIApplicationLaunchOptionsURLKey", "URL", "url", NULL
    };
    int i;
    for (i = 0; keys[i]; i++) {
        if (is_voice(dict_get(dict, keys[i]))) return 1;
    }
    return 0;
}

static int scene_is_wetype(id scene) {
    if (!scene) return 0;
    id proc = msg0(scene, "clientProcess");
    if (proc && is_wetype(msg0(proc, "bundleIdentifier"))) return 1;
    if (is_wetype(msg0(scene, "identifier"))) return 1;
    id ident = msg0(scene, "identity");
    if (ident && is_wetype(msg0(ident, "identifier"))) return 1;
    return 0;
}

/* ---------- workspace facts ---------- */

static void frontmost_bundle(char *buf, size_t buflen) {
    buf[0] = '\0';
    Class SBW = objc_getClass("SBMainWorkspace");
    if (!SBW) return;
    if (!cls0(SBW, "_instanceIfExists") && !cls0(SBW, "sharedInstance")) return;
    id app = cls0(objc_getClass("UIApplication"), "sharedApplication");
    id sbApp = msg0(app, "_accessibilityFrontMostApplication");
    const char *p = cstr(msg0(sbApp, "bundleIdentifier"));
    if (p && p[0]) snprintf(buf, buflen, "%s", p);
}

static int frontmost_is_wetype(void) {
    char buf[128];
    frontmost_bundle(buf, sizeof(buf));
    return is_wetype_str(buf);
}

static int ui_locked(void) {
    id sh = cls0(objc_getClass("SBLockScreenManager"), "sharedInstance");
    if (!sh || !resp(sh, "isUILocked")) return 0;
    return ((BOOL (*)(id, SEL))objc_msgSend)(sh, sel_registerName("isUILocked")) ? 1 : 0;
}

static pid_t wxkb_pid(void) {
    id sh = cls0(objc_getClass("FBSSystemService"), "sharedService");
    if (!resp(sh, "pidForApplication:")) return 0;
    return (pid_t)((int (*)(id, SEL, id))objc_msgSend)(
        sh, sel_registerName("pidForApplication:"), nsstr(kWeTypeBundle));
}

/* WeType hands the recognised text back to the app named as the launch source.
 * SpringBoard itself knows that app: at kick time the frontmost process is the
 * host the keyboard is typing into. (The keyboard extension cannot publish it
 * itself - its sandbox rejects writes to /var/mobile/Library/Preferences, which
 * is also why the pending URL file never actually existed.) */
static void remember_voice_host(void) {
    char fb[128];
    fb[0] = 0;
    gVoiceHost[0] = 0;
    frontmost_bundle(fb, sizeof(fb));
    if (!fb[0]) { wlog("host: frontmost unknown"); return; }
    if (is_wetype(nsstr(fb)) || strstr(fb, "springboard")) {
        wlog("host: ignored %s", fb);
        return;
    }
    snprintf(gVoiceHost, sizeof(gVoiceHost), "%s", fb);
    wlog("host: %s", gVoiceHost);
}

static void read_voice_url(char *out, size_t n) {
    snprintf(out, n, "%s", kVoiceURL);
    FILE *f = fopen(kPendingFile, "r");
    if (!f) return;
    char tmp[256];
    if (fgets(tmp, (int)sizeof(tmp), f)) {
        size_t len = strlen(tmp);
        while (len > 0 && (tmp[len - 1] == '\n' || tmp[len - 1] == '\r')) tmp[--len] = '\0';
        if (len > 0 && strstr(tmp, "wetype:")) snprintf(out, n, "%s", tmp);
    }
    fclose(f);
}

/* ---------- scene pinning ---------- */

static void pin_bg(id settings) {
    if (!settings) return;
    if (resp(settings, "setForeground:"))
        ((void (*)(id, SEL, BOOL))objc_msgSend)(
            settings, sel_registerName("setForeground:"), NO);
    if (resp(settings, "setBackgrounded:"))
        ((void (*)(id, SEL, BOOL))objc_msgSend)(
            settings, sel_registerName("setBackgrounded:"), YES);
    if (resp(settings, "setUnderLock:"))
        ((void (*)(id, SEL, BOOL))objc_msgSend)(
            settings, sel_registerName("setUnderLock:"), NO);
    if (resp(settings, "setDeactivationReasons:"))
        ((void (*)(id, SEL, unsigned long long))objc_msgSend)(
            settings, sel_registerName("setDeactivationReasons:"), 0ull);
}

/* A token is only ever held for a voice-initiated open, so an icon tap
 * (no token) falls through untouched and WeType foregrounds normally. */
static int should_pin(id scene) {
    if (disabled()) return 0;
    if (!token_valid()) return 0;
    if (!scene_is_wetype(scene)) return 0;
    if (ui_locked()) return 0;
    if (now_s() < gQuietUntil) return 0;
    return 1;
}

static id pinned_copy(id settings) {
    id m = msg0(settings, "mutableCopy");
    if (!m) return nil;
    pin_bg(m);
    return m;
}

static void retire_scene(id scene) {
    id mgr = cls0(objc_getClass("FBSceneManager"), "sharedInstance");
    SEL sel = sel_registerName("_applyMutableSettings:toScene:withTransitionContext:completion:");
    if (!resp(mgr, "_applyMutableSettings:toScene:withTransitionContext:completion:")) return;
    id m = pinned_copy(msg0(scene, "settings"));
    if (!m) {
        wlog("retire skipped: no mutable settings");
        return;
    }
    gQuietUntil = now_s() + 0.6;
    void (^done)(void) = ^{ wlog("retire applied"); };
    ((void (*)(id, SEL, id, id, id, id))objc_msgSend)(mgr, sel, m, scene, nil,
                                                      (id)_Block_copy(done));
    hb("retire");
    wlog("retire wetype scene pid=%d", (int)wxkb_pid());
}

/* ---------- official-request fallbacks ---------- */

static id mutate_suspended(id opts) {
    id dict = nil;
    int was_fbs = 0;
    if (opts && resp(opts, "dictionary")) {
        dict = msg0(opts, "dictionary");
        was_fbs = 1;
    } else if (opts) {
        dict = opts;
    }

    Class MD = objc_getClass("NSMutableDictionary");
    id md = nil;
    if (dict && resp(dict, "mutableCopy"))
        md = msg0(dict, "mutableCopy");
    if (!md && MD)
        md = ((id (*)(id, SEL))objc_msgSend)((id)MD, sel_registerName("dictionary"));
    if (!md) return opts;

    id yes = nsnum_yes();
    dict_set(md, "__ActivateSuspended", yes);
    dict_set(md, "ActivateSuspended", yes);
    dict_set(md, "LSApplicationLaunchOptionActivateSuspended", yes);

    if (was_fbs) {
        Class Opt = objc_getClass("FBSOpenApplicationOptions");
        SEL mk = sel_registerName("optionsWithDictionary:");
        if (Opt && ((BOOL (*)(id, SEL, SEL))objc_msgSend)(
                (id)Opt, sel_registerName("respondsToSelector:"), mk)) {
            id neu = ((id (*)(id, SEL, id))objc_msgSend)((id)Opt, mk, md);
            if (neu) return neu;
        }
        if (opts && resp(opts, "setDictionary:")) {
            ((void (*)(id, SEL, id))objc_msgSend)(
                opts, sel_registerName("setDictionary:"), md);
            return opts;
        }
    }
    return md;
}

static void take_token_if_mine(const char *why) {
    if (token_valid()) return;
    if (frontmost_is_wetype()) {
        wlog("token refused: wetype frontmost (%s)", why);
        return;
    }
    token_grant(why);
}

static void repl_fbs_openurl(id self, SEL cmd, id url, id app, id opts,
                             unsigned int port, id result) {
    if (is_voice(url)) {
        mark_voice("FBS openURL");
        take_token_if_mine("FBS openURL");
        opts = mutate_suspended(opts);
        wlog("mutate FBS openURL %s", cstr(url));
    }
    if (OrigFBSOpenURL) OrigFBSOpenURL(self, cmd, url, app, opts, port, result);
}

static void repl_fbs_openapp(id self, SEL cmd, id app, id opts,
                             unsigned int port, id result) {
    if (is_wetype(app) && (opts_is_voice(opts) || in_voice())) {
        mark_voice("FBS openApp");
        take_token_if_mine("FBS openApp");
        opts = mutate_suspended(opts);
        wlog("mutate FBS openApp %s", cstr(app));
    }
    if (OrigFBSOpenApp) OrigFBSOpenApp(self, cmd, app, opts, port, result);
}

static BOOL repl_allow(id self, SEL cmd, id app) {
    if (is_wetype(app)) return YES;
    return OrigAllow ? OrigAllow(self, cmd, app) : NO;
}

static void repl_fb_open(id self, SEL cmd, id app, id opts, id origin, id req, id comp) {
    if (is_wetype(app) && (opts_is_voice(opts) || in_voice())) {
        mark_voice("FB openApp");
        take_token_if_mine("FB openApp");
        opts = mutate_suspended(opts);
        wlog("mutate FB openApp %s", cstr(app));
    }
    if (OrigFBOpen) OrigFBOpen(self, cmd, app, opts, origin, req, comp);
}

/* iOS 16.1.2 has no FBSceneManager _applyMutableSettings: any more; the live
 * entry points are the FBScene update/perform families. Hook all of them and
 * report by heartbeat which one a WeType transition actually goes through. */
/* A process launched *into* a suppressed scene never finishes activating: it is
 * suspended and reaped within ~1.5s, so recording cannot start. Cold launches
 * therefore get activated normally and are retired a moment later; once the app
 * has proven it is alive, later transitions are suppressed inline (no flash). */
static void pin_via_block(id scene, const char *why) {
    if (!resp(scene, "updateSettingsWithBlock:")) {
        hb("noblockapi");
        wlog("no block API on scene (%s)", why);
        return;
    }
    void (^blk)(id) = ^(id st) { pin_bg(st); };
    ((void (*)(id, SEL, id))objc_msgSend)(
        scene, sel_registerName("updateSettingsWithBlock:"), (id)_Block_copy(blk));
    hb("pin");
    wlog("pinned via block (%s)", why);
}

static void schedule_retire(id scene) {
    if (gTokRetired) return;
    gTokRetired = 1;
    id held = ((id (*)(id, SEL))objc_msgSend)(scene, sel_registerName("retain"));
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.25 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        if (token_valid() && resp(held, "updateSettingsWithBlock:")) {
            pin_via_block(held, "retire");
            wlog("retired wetype scene after activation");
        }
        ((void (*)(id, SEL))objc_msgSend)(held, sel_registerName("release"));
    });
}

/* Handing SpringBoard a mutableCopy of its own argument crashed it: on iOS 16
 * the first parameter of updateSettings:withTransitionContext: is not a
 * FBSSceneSettings we may replace. Observe only, and suppress through the
 * block-based API, which mutates the object FrontBoard itself provides. */
static id gLastScene;

/* The file is plain C (no ARC), so keep the scene alive by hand: an async
 * notify must never message a scene that FrontBoard already dropped. */
static void remember_scene(id scene) {
    if (gLastScene == scene) return;
    if (gLastScene)
        ((void (*)(id, SEL))objc_msgSend)(gLastScene, sel_registerName("release"));
    gLastScene = scene
        ? ((id (*)(id, SEL))objc_msgSend)(scene, sel_registerName("retain")) : nil;
}

static void note_transition(id scene, const char *evt) {
    if (!scene_is_wetype(scene)) return;
    remember_scene(scene);
    hb(evt);
    if (!token_valid()) return;
    if (ui_locked()) return;
    if (!gActivated) {
        gActivated = 1;
        schedule_retire(scene);
        /* -[FBSceneManager _applyMutableSettings:toScene:…] does not exist on
         * iOS 16, so the scheduled retire was a silent no-op and the app stayed
         * visible until a later transition reached the block API. Suppress on
         * the first one instead; the 0.7.5 run proved the ASR keeps working
         * once the scene is backgrounded. */
        /* Do not suppress here: pinning before the capture engine produces its
         * first packet leaves setActive answering ok while the engine delivers
         * nothing (0.7.6 and 0.7.7 both saw zero PCM). The host tweak posts
         * com.wxkb.host.pcm1 the moment audio really flows, and that pins. */
        wlog("allow activation, retire scheduled (%s)", evt);
        return;
    }
    if (now_s() < gQuietUntil) return;
    gQuietUntil = now_s() + 0.6;
    pin_via_block(scene, evt);
}

static void repl_scene_up3(id self, SEL cmd, id settings, id ctx, id comp) {
    if (OrigSceneUp3) OrigSceneUp3(self, cmd, settings, ctx, comp);
    note_transition(self, "e3");
}

static void repl_scene_up2(id self, SEL cmd, id settings, id ctx) {
    if (OrigSceneUp2) OrigSceneUp2(self, cmd, settings, ctx);
    note_transition(self, "e2");
}

static void repl_scene_perf(id self, SEL cmd, id block) {
    if (scene_is_wetype(self)) {
        hb("ep");
        if (should_pin(self) && block) {
            void (^orig)(id) = (void (^)(id))block;
            void (^w)(id) = ^(id settings) {
                if (orig) orig(settings);
                pin_bg(settings);
                hb("pinblk");
            };
            if (OrigScenePerf) { OrigScenePerf(self, cmd, (id)_Block_copy(w)); return; }
        }
    }
    if (OrigScenePerf) OrigScenePerf(self, cmd, block);
}

static void repl_scene_perf2(id self, SEL cmd, id block, id comp) {
    if (scene_is_wetype(self)) {
        hb("ep2");
        if (should_pin(self) && block) {
            void (^orig)(id) = (void (^)(id))block;
            void (^w)(id) = ^(id settings) {
                if (orig) orig(settings);
                pin_bg(settings);
                hb("pinblk");
            };
            if (OrigScenePerf2) { OrigScenePerf2(self, cmd, (id)_Block_copy(w), comp); return; }
        }
    }
    if (OrigScenePerf2) OrigScenePerf2(self, cmd, block, comp);
}

/* Central settings commit point on older iOS (bakgrunnur era). */
static void repl_mgr_apply(id self, SEL cmd, id settings, id scene, id ctx, id comp) {
    hb("tryapply");
    if (should_pin(scene)) {
        id m = pinned_copy(settings);
        if (m) {
            settings = m;
            hb("pin");
            wlog("pin apply wetype fg->bg pid=%d", (int)wxkb_pid());
        }
    }
    if (OrigMgrApply) OrigMgrApply(self, cmd, settings, scene, ctx, comp);
}

static void repl_mgr_note_fg(id self, SEL cmd, id scene) {
    hb("notefg");
    if (OrigMgrNoteFG) OrigMgrNoteFG(self, cmd, scene);
    if (!scene_is_wetype(scene)) return;
    char fb[128];
    frontmost_bundle(fb, sizeof(fb));
    wlog("noteFG wetype frontmost=%s token=%d", fb, token_valid());
    if (!gTokRetired && should_pin(scene)) {
        gTokRetired = 1;
        retire_scene(scene);
    }
}

/* Fallback when the manager commit point is absent on this iOS build. */
static void repl_scene_block(id self, SEL cmd, id block) {
    if (should_pin(self) && block) {
        void (^origBlock)(id) = (void (^)(id))block;
        void (^wrapped)(id) = ^(id settings) {
            if (origBlock) origBlock(settings);
            pin_bg(settings);
        };
        if (OrigSceneBlock) OrigSceneBlock(self, cmd, (id)wrapped);
        return;
    }
    if (OrigSceneBlock) OrigSceneBlock(self, cmd, block);
}

/* ---------- SpringBoard-side suspended launch ---------- */

static int wks_launch_workspace(const char *bid, const char *urlstr) {
    Class PM = objc_getClass("FBProcessManager");
    Class RQ = objc_getClass("FBSystemServiceOpenApplicationRequest");
    Class OP = objc_getClass("FBSOpenApplicationOptions");
    Class SY = objc_getClass("FBSystemService");
    Class WB = objc_getClass("SBMainWorkspace");
    if (!PM || !RQ || !OP || !SY || !WB) {
        wlog("ws classes missing PM=%p RQ=%p OP=%p SY=%p WB=%p",
             (void *)PM, (void *)RQ, (void *)OP, (void *)SY, (void *)WB);
        hb("wcls");
        return 0;
    }

    id pm = cls0(PM, "sharedInstance");
    id procs = msg1(pm, "applicationProcessesForBundleIdentifier:", nsstr(kSpringBoardBundle));
    id sbProc = msg0(procs, "firstObject");
    if (!sbProc) {
        wlog("no springboard FBApplicationProcess");
        hb("wproc");
        return 0;
    }

    id req = nil;
    /* +[FBSystemServiceOpenApplicationRequest request] answers nil on this
     * build, which used to drop the whole launch to the foregrounding tier.
     * Try every documented constructor and say which one took. */
    const char *ctors[] = { "request", "openApplicationRequest",
                            "defaultRequest", "new", NULL };
    const char *ctorTags[] = { "reqa", "reqb", "reqc", "reqd", NULL };
    for (int i = 0; ctors[i]; i++) {
        if (!cresp(RQ, ctors[i])) continue;
        req = ((id (*)(id, SEL))objc_msgSend)(
            (id)RQ, sel_registerName(ctors[i]));
        if (req) { hb(ctorTags[i]); break; }
    }
    if (!req) {
        id alloc = ((id (*)(id, SEL))objc_msgSend)(
            (id)RQ, sel_registerName("alloc"));
        if (alloc && resp(alloc, "init"))
            req = ((id (*)(id, SEL))objc_msgSend)(alloc, sel_registerName("init"));
        if (req) hb("reqinit");
    }
    if (!req) {
        wlog("no FBSystemServiceOpenApplicationRequest");
        hb("wreq");
        return 0;
    }
    call1(req, "setClientProcess:", sbProc);
    if (resp(req, "setTrusted:"))
        ((void (*)(id, SEL, char))objc_msgSend)(req, sel_registerName("setTrusted:"), (char)1);
    call1(req, "setBundleIdentifier:", nsstr(bid));

    id dict = nsnewdict();
    if (!dict) { hb("wdict"); return 0; }
    id yes = nsnum_yes();
    dict_set(dict, "__ActivateSuspended", yes);
    dict_set(dict, "processLaunchIntent", nsnum_int(4));
    dict_set(dict, "__SBWorkspaceOpenOptionUnlockResult", nsnum_int(1));
    dict_set(dict, "__PromptUnlockDevice", yes);
    dict_set(dict, "__UnlockDevice", yes);
    id payload = nsnewdict();
    if (payload) {
        dict_set(payload, "UIApplicationLaunchOptionsSourceApplicationKey",
                 nsstr(gVoiceHost[0] ? gVoiceHost : kSpringBoardBundle));
        dict_set(dict, "__PayloadOptions", payload);
    }
    /* The launch-time URL is the one condition that correlates with the mic
     * actually streaming: 0.7.5 (URL at launch) delivered PCM for 60s and text,
     * while every run without it delivered zero frames - including 0.7.7, which
     * really was foregrounded and had an in-process openURL called on it. It is
     * passed as a launch option so WeType initialises itself as a voice session
     * instead of reacting to a late URL open. */
    if (urlstr && urlstr[0]) {
        dict_set(dict, "__PayloadURL", nsstr(urlstr));
        dict_set(dict, "UIApplicationLaunchOptionsURLKey", nsstr(urlstr));
    }

    id opts = nil;
    if (cresp(OP, "optionsWithDictionary:"))
        opts = ((id (*)(id, SEL, id))objc_msgSend)(
            (id)OP, sel_registerName("optionsWithDictionary:"), dict);
    if (!opts) opts = dict;
    /* +optionsWithDictionary: runs -_sanitizeAndValidatePayload, which drops
     * keys it does not know — including __ActivateSuspended — so the workspace
     * happily launched the app in the foreground. Re-apply the raw dictionary
     * through the plain setter, which does not sanitize. */
    if (opts != dict && resp(opts, "setDictionary:")) {
        ((void (*)(id, SEL, id))objc_msgSend)(
            opts, sel_registerName("setDictionary:"), dict);
        hb("resan");
    }
    call1(req, "setOptions:", opts);

    id sysSvc = cls0(SY, "sharedInstance");
    id ws = cls0(WB, "sharedInstance");
    if (!ws) ws = cls0(WB, "_instanceIfExists");
    if (!sysSvc || !ws) {
        wlog("no FBSystemService sharedInstance / SBMainWorkspace");
        hb("wws");
        return 0;
    }
    SEL sel = sel_registerName("systemService:handleOpenApplicationRequest:withCompletion:");
    if (!resp(ws, "systemService:handleOpenApplicationRequest:withCompletion:")) {
        wlog("workspace handle selector missing");
        hb("wsel");
        return 0;
    }
    void (^done)(id) = ^(id err) { wlog("ws open err=%s", cstr(err)); };
    ((void (*)(id, SEL, id, id, id))objc_msgSend)(ws, sel, sysSvc, req, (id)_Block_copy(done));
    hb("open");
    wlog("ws open issued bid=%s url=%s", bid, urlstr && urlstr[0] ? urlstr : "-");
    return 1;
}

static const char *wks_launch_suspended(const char *bid, const char *urlstr) {
    if (wks_launch_workspace(bid, urlstr)) { hb("t1ws"); return "workspace"; }
    /* SpringBoard cannot write a log file here, so the tier that actually ran
     * only survives as a heartbeat the root daemon timestamps. */
    hb("t1fail");

    id sh = cls0(objc_getClass("FBSSystemService"), "sharedService");
    if (sh && resp(sh, "createClientPort") &&
        resp(sh, "openApplication:options:clientPort:withResult:")) {
        unsigned int port = ((unsigned int (*)(id, SEL))objc_msgSend)(
            sh, sel_registerName("createClientPort"));
        id dict = nsnewdict();
        id yes = nsnum_yes();
        dict_set(dict, "__ActivateSuspended", yes);
        dict_set(dict, "ActivateSuspended", yes);
        dict_set(dict, "LSApplicationLaunchOptionActivateSuspended", yes);
        /* same reason as the workspace tier */
        (void)urlstr;
        void (^done)(id) = ^(id r) { wlog("fbs open result=%s", cstr(r)); };
        ((void (*)(id, SEL, id, id, unsigned int, id))objc_msgSend)(
            sh, sel_registerName("openApplication:options:clientPort:withResult:"),
            nsstr(bid), mutate_suspended(dict), port, (id)_Block_copy(done));
        wlog("fbs open issued");
        hb("t2fbs");
        return "fbs";
    }

    /* The daemon watches the same notify and launches after its own delay. */
    hb("t3daemon");
    return "daemon";
}

/* ---------- notify plumbing ---------- */

static void on_voice_launch(int t) {
    (void)t;
    if (disabled()) return;
    double now = now_s();
    if ((now - gLastKick) < 4.0) {
        wlog("notify launch deduped");
        return;
    }
    gLastKick = now;
    wlog("notify launch pid=%d", (int)wxkb_pid());
    mark_voice("notify");

    if (wxkb_pid() > 0) {
        take_token_if_mine("already-up");
        wlog("host already up — pin only");
        hb("pinned");
        return;
    }
    if (frontmost_is_wetype()) {
        wlog("token refused: wetype already frontmost");
        return;
    }
    if (ui_locked()) {
        wlog("token refused: ui locked");
        return;
    }
    /* Grant before opening: the transition to pin can start immediately. */
    token_grant("pre-launch");
    remember_voice_host();
    char url[256];
    read_voice_url(url, sizeof(url));
    wlog("launch chain=%s", wks_launch_suspended(kWeTypeBundle, url));
}

static void on_host_ready(int t) {
    (void)t;
    /* Producer is up and recording; stop pinning so a real icon tap stays front. */
    token_clear("host-ready");
}

static void on_pcm_first(int t) {
    (void)t;
    if (disabled() || !token_valid()) return;
    if (!gLastScene) { wlog("pcm1 without a known scene"); return; }
    gQuietUntil = now_s() + 0.6;
    pin_via_block(gLastScene, "pcm1");
    wlog("pinned on first pcm");
}

static void register_notify(void) {
    if (gNotifyRegistered) return;
    int tok = 0, tok2 = 0;
    if (notify_register_dispatch(kNotifyLaunch, &tok, dispatch_get_main_queue(),
                                 ^(int t) { on_voice_launch(t); }) == 0) {
        gNotifyRegistered = 1;
        wlog("notify registered %s", kNotifyLaunch);
    } else {
        wlog("notify register FAILED %s", kNotifyLaunch);
    }
    notify_register_dispatch(kNotifyReady, &tok2, dispatch_get_main_queue(),
                             ^(int t) { on_host_ready(t); });
    static int tok3 = 0;
    notify_register_dispatch("com.wxkb.host.pcm1", &tok3, dispatch_get_main_queue(),
                             ^(int t) { on_pcm_first(t); });
}

/* ---------- install ---------- */

static void hook1(const char *clsname, const char *selname, IMP neu, IMP *orig) {
    Class cls = objc_getClass(clsname);
    SEL sel = sel_registerName(selname);
    if (!cls || !class_getInstanceMethod(cls, sel)) {
        wlog("skip %s %s", clsname, selname);
        return;
    }
    if (orig && *orig) return;
    MSHookMessageEx(cls, sel, neu, orig);
    wlog("hooked %s %s", clsname, selname);
}

static void install_once(void) {
    hook1("FBSSystemService",
          "openURL:application:options:clientPort:withResult:",
          (IMP)repl_fbs_openurl, (IMP *)&OrigFBSOpenURL);
    hook1("FBSSystemService",
          "openApplication:options:clientPort:withResult:",
          (IMP)repl_fbs_openapp, (IMP *)&OrigFBSOpenApp);
    hook1("FBSystemService",
          "_isAllowListedLaunchSuspendedApp:",
          (IMP)repl_allow, (IMP *)&OrigAllow);
    hook1("FBSystemService",
          "openApplication:withOptions:originator:requestID:completion:",
          (IMP)repl_fb_open, (IMP *)&OrigFBOpen);

    if (cls_has("FBSceneManager",
               "_applyMutableSettings:toScene:withTransitionContext:completion:")) {
        hook1("FBSceneManager",
              "_applyMutableSettings:toScene:withTransitionContext:completion:",
              (IMP)repl_mgr_apply, (IMP *)&OrigMgrApply);
        hook1("FBSceneManager",
              "_noteSceneMovedToForeground:",
              (IMP)repl_mgr_note_fg, (IMP *)&OrigMgrNoteFG);
        gApplyHooked = 1;
        if (!gApplyReported) { gApplyReported = 1; hb("applyok"); }
    } else if (!gApplyReported) {
        gApplyReported = 1;
        hb("applymissing");
    }

    /* These are the live entry points on iOS 15/16. */
    hook1("FBScene", "updateSettings:withTransitionContext:completion:",
          (IMP)repl_scene_up3, (IMP *)&OrigSceneUp3);
    hook1("FBScene", "updateSettings:withTransitionContext:",
          (IMP)repl_scene_up2, (IMP *)&OrigSceneUp2);
    hook1("FBScene", "updateSettingsWithBlock:",
          (IMP)repl_scene_block, (IMP *)&OrigSceneBlock);
    hook1("FBScene", "performUpdate:withCompletion:",
          (IMP)repl_scene_perf2, (IMP *)&OrigScenePerf2);
    hook1("FBScene", "performUpdate:",
          (IMP)repl_scene_perf, (IMP *)&OrigScenePerf);
    if (!gSceneReported && (OrigSceneUp3 || OrigSceneUp2 || OrigSceneBlock ||
        OrigScenePerf || OrigScenePerf2)) {
        gSceneReported = 1;
        hb("scenehooks");
    }

    if (!gProbed) {
        gProbed = 1;
        wlog("probe kbfocus=%d",
             cresp(objc_getClass("FBSSystemService"),
                   "setKeyboardFocusApplicationPID:completion:") ? 1 : 0);
    }

    if (!gInstalled && (OrigFBSOpenURL || OrigMgrApply || OrigAllow || OrigSceneBlock)) {
        gInstalled = 1;
        hb("ready");
    }
    register_notify();
}

static void boot_guard(int installed) {
    if (installed) {
        unlink(kBootsFile);
        return;
    }
    int n = 1;
    FILE *f = fopen(kBootsFile, "r");
    if (f) {
        if (fscanf(f, "%d", &n) != 1) n = 1;
        fclose(f);
        n++;
    }
    f = fopen(kBootsFile, "w");
    if (f) {
        fprintf(f, "%d\n", n);
        fclose(f);
    }
    wlog("nothing hooked, boot=%d", n);
    if (n >= 3) {
        f = fopen(kOffFile, "w");
        if (f) {
            fprintf(f, "no hooks after %d boots\n", n);
            fclose(f);
        }
        wlog("self-disabled via %s", kOffFile);
    }
}

static void *install_thr(void *ctx) {
    (void)ctx;
    int i;
    for (i = 0; i < 50; i++) {
        if (objc_getClass("FBSystemService") && objc_getClass("FBSSystemService"))
            break;
        usleep(100000);
    }
    install_once();
    for (i = 0; i < 8; i++) {
        sleep(1);
        install_once();
    }
    wlog("install done apply=%d scene=%p allow=%p fbsURL=%p installed=%d",
         gApplyHooked, (void *)OrigSceneBlock, (void *)OrigAllow,
         (void *)OrigFBSOpenURL, gInstalled);
    boot_guard(gInstalled);
    return NULL;
}

__attribute__((constructor))
static void ctor(void) {
    const char *pn = getprogname();
    /* Filesystem logging is unreliable here: sandboxed/confined processes
     * cannot write the shared preference paths, so a missing log file proves
     * nothing. The root daemon observes this notify instead. */
    notify_post("com.wxkb.sblaunch.hello");
    wlog("v0.7.12 ctor pn=%s", pn ? pn : "?");
    if (disabled()) {
        wlog("disabled, exit");
        return;
    }
    pthread_t th;
    if (pthread_create(&th, NULL, install_thr, NULL) == 0)
        pthread_detach(th);
}
