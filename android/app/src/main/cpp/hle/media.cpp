// AVAudioSession, MPMoviePlayerController, MPMusicPlayerController and
// MPMediaPickerController. The game ships no videos and the iPod library does
// not exist on Android, so these report "nothing to play".
#include "hle/media.h"

#include "hle/foundation.h"
#include "hle/nsstring.h"
#include "objc/runtime.h"

namespace media {

namespace {

id g_MPMoviePlayerPlaybackDidFinishNotification = nil;
id g_session = nil, g_music_player = nil;

id Session_shared(Class cls, SEL) {
    if (!g_session) g_session = objc::alloc_instance(cls);
    return g_session;
}
BOOL Session_setCategory(id, SEL, id cat, id* err) {
    LOGI("AVAudioSession setCategory:%s", cat ? ns::to_utf8(cat).c_str() : "nil");
    if (err) *err = nil;
    return YES;
}
BOOL Session_setActive(id, SEL, BOOL, id* err) {
    if (err) *err = nil;
    return YES;
}
BOOL Session_setActiveOpts(id, SEL, BOOL, unsigned long, id* err) {
    if (err) *err = nil;
    return YES;
}
BOOL Session_otherAudioPlaying(id, SEL) { return NO; }
void Session_setDelegate(id, SEL, id) {}

struct MovieObj {
    Class isa;
    id url;
};
id Movie_initWithContentURL(id self, SEL, id url) {
    ((MovieObj*)self)->url = objc::retain(url);
    LOGI("MPMoviePlayerController %s", foundation::url_string(url).c_str());
    return self;
}
void Movie_dealloc(id self, SEL) {
    objc::release(((MovieObj*)self)->url);
    objc::dispose_instance(self);
}
void Movie_play(id self, SEL) {
    // No video playback: report completion on the next run loop pass.
    objc::retain(self);
    foundation::run_on_main(
        [self]() {
            foundation::post_notification(g_MPMoviePlayerPlaybackDidFinishNotification, self);
            objc::release(self);
        },
        false);
}
void Movie_noop(id, SEL) {}
void Movie_noopL(id, SEL, long) {}
void Movie_noopB(id, SEL, BOOL) {}
id Movie_view(id, SEL) { return nil; }

id Music_iPodMusicPlayer(Class cls, SEL) {
    if (!g_music_player) g_music_player = objc::alloc_instance(cls);
    return g_music_player;
}
long Music_playbackState(id, SEL) { return 0; }  // stopped
id Music_nil(id, SEL) { return nil; }
void Music_noop(id, SEL) {}
void Music_noop1(id, SEL, id) {}

// MPMediaPickerController: cancel right after being shown.
struct PickerObj {
    Class isa;
    void* vc;  // UIViewController data
    uint8_t pad[400 - 16];
    id delegate;
};
id Picker_initWithMediaTypes(id self, SEL, unsigned long) { return objc::send(self, SEL_("init")); }
void Picker_setDelegate(id self, SEL, id d) { ((PickerObj*)self)->delegate = d; }
void Picker_viewDidAppear(id self, SEL, BOOL) {
    id d = ((PickerObj*)self)->delegate;
    if (d && objc::responds_to(d->isa, SEL_("mediaPickerDidCancel:"))) {
        objc::retain(self);
        foundation::run_on_main(
            [self, d]() {
                objc::send<void>(d, SEL_("mediaPickerDidCancel:"), self);
                objc::release(self);
            },
            false);
    }
}

}  // namespace

void init() {
    using objc::add_class_method;
    using objc::add_method;
    g_MPMoviePlayerPlaybackDidFinishNotification = ns::make_constant("MPMoviePlayerPlaybackDidFinishNotification");
    hle::register_symbol("_MPMoviePlayerPlaybackDidFinishNotification", &g_MPMoviePlayerPlaybackDidFinishNotification);

    Class S = objc::define_class("AVAudioSession", "NSObject", 8);
    add_class_method(S, "sharedInstance", (void*)Session_shared);
    add_method(S, "setCategory:error:", (void*)Session_setCategory);
    add_method(S, "setActive:error:", (void*)Session_setActive);
    add_method(S, "setActive:withOptions:error:", (void*)Session_setActiveOpts);
    add_method(S, "isOtherAudioPlaying", (void*)Session_otherAudioPlaying);
    add_method(S, "secondaryAudioShouldBeSilencedHint", (void*)Session_otherAudioPlaying);
    add_method(S, "setDelegate:", (void*)Session_setDelegate);

    Class M = objc::define_class("MPMoviePlayerController", "NSObject", sizeof(MovieObj));
    add_method(M, "initWithContentURL:", (void*)Movie_initWithContentURL);
    add_method(M, "dealloc", (void*)Movie_dealloc);
    add_method(M, "play", (void*)Movie_play);
    add_method(M, "stop", (void*)Movie_noop);
    add_method(M, "pause", (void*)Movie_noop);
    add_method(M, "prepareToPlay", (void*)Movie_noop);
    add_method(M, "view", (void*)Movie_view);
    for (const char* s : {"setScalingMode:", "setControlStyle:", "setMovieControlMode:", "setRepeatMode:"})
        add_method(M, s, (void*)Movie_noopL);
    for (const char* s : {"setFullscreen:", "setShouldAutoplay:", "setUseApplicationAudioSession:"})
        add_method(M, s, (void*)Movie_noopB);

    Class MP = objc::define_class("MPMusicPlayerController", "NSObject", 8);
    add_class_method(MP, "iPodMusicPlayer", (void*)Music_iPodMusicPlayer);
    add_class_method(MP, "applicationMusicPlayer", (void*)Music_iPodMusicPlayer);
    add_class_method(MP, "systemMusicPlayer", (void*)Music_iPodMusicPlayer);
    add_method(MP, "playbackState", (void*)Music_playbackState);
    add_method(MP, "nowPlayingItem", (void*)Music_nil);
    for (const char* s : {"play", "pause", "stop", "skipToNextItem", "skipToPreviousItem",
                          "beginGeneratingPlaybackNotifications", "endGeneratingPlaybackNotifications"})
        add_method(MP, s, (void*)Music_noop);
    add_method(MP, "setQueueWithItemCollection:", (void*)Music_noop1);
    add_method(MP, "setQueueWithQuery:", (void*)Music_noop1);

    Class MPick = objc::define_class("MPMediaPickerController", "UIViewController", sizeof(PickerObj));
    add_method(MPick, "initWithMediaTypes:", (void*)Picker_initWithMediaTypes);
    add_method(MPick, "setDelegate:", (void*)Picker_setDelegate);
    add_method(MPick, "setAllowsPickingMultipleItems:", (void*)Movie_noopB);
    add_method(MPick, "setPrompt:", (void*)Music_noop1);
    add_method(MPick, "viewDidAppear:", (void*)Picker_viewDidAppear);
}

}  // namespace media
