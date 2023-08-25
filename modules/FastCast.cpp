#include <windows.h>
#include <commctrl.h>
#include <list>
#include "Define.h"
#include "d2h_module.hpp"
#include "d2ptrs.h"
#include "KeyModule.hpp"
#include "d2vars.h"
#include "log.h"
#include "D2Utils.hpp"
#include "Define.h"
#include "Actor.hpp"
#include "D2CallStub.hpp"
#include "Event.hpp"
#include "cConfigLoader.hpp"
#include "hotkey.hpp"

struct SkillTime {
    int         key;
    int         skillId;
    uint64_t    startTime;

    bool        isExpired() const
    {
        return startTime + 1000 < GetTickCount64();
    }

    bool        isValid() const
    {
        return skillId != -1;
    }
};

//#define DEBUG_FAST_CAST

#ifdef DEBUG_FAST_CAST
#define fcDbg(fmt, ...)         D2Util::showInfo(fmt, ##__VA_ARGS__)
#define trace(fmt, ...)         do { log_trace(fmt, ##__VA_ARGS__); log_flush(); } while (0)
#else
#define fcDbg(fmt, ...)
#define trace(fmt, ...)         log_verbose(fmt, ##__VA_ARGS__)
#endif


static bool fastCastEnabled;
static uint32_t fastCastToggleKey = Hotkey::Invalid;

static WNDPROC getOrigProc(HWND hwnd)
{
    TCHAR       className[64];
    int         classNameLen;
    WNDCLASS    classInfo;
    HINSTANCE   hInst = GetModuleHandle(NULL);
    WNDPROC     orig;

    if (hInst == NULL) {
        return NULL;
    }
    // Get "Diablo II" class name
    classNameLen = GetClassName(hwnd, &className[0], sizeof(className) / sizeof(className[0]));
    if (classNameLen == 0) {
        trace("Failed to get class name\n");
        return FALSE;
    }
    if (!GetClassInfo(hInst, className, &classInfo)) {
        trace("Failed to get class info\n");
        return FALSE;
    }
    orig = classInfo.lpfnWndProc;
    trace("Orig proc: %p\n", orig);
    return orig;
}

__declspec(noinline)
LRESULT WINAPI mySendMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    static WNDPROC      origProc;
    static BOOL         isGot;

    LRESULT             ret;

    trace("Get message 0x%x param 0x%x 0x%x\n", msg, wParam, lParam);
    if (!isGot) {
        origProc = getOrigProc(hWnd);
        isGot = TRUE;
    }
    if (origProc != NULL) {
        ret = CallWindowProcA(origProc, hWnd, msg, wParam, lParam);
    } else {
        ret = SendMessageA(hWnd, msg, wParam, lParam);
    }

    return ret;
}

static bool isFastCastAble()
{
    if (!fastCastEnabled) {
        return false;
    }
    return D2Util::isGameScreen();
}

class Watcher {
public:
    virtual void signal(Actor * actor) = 0;
};

class RunActor: public Actor {
public:
    RunActor(Watcher * watcher): Actor(), mWatcher(watcher), mIsRunning(false)
    {
    }

    void startSkill(const SkillTime& skill)
    {
        mIsRunning = true;
        mSkill = skill;
        run();
    }

    void cancel()
    {
        if (mIsRunning) {
            mTimer.stop();
            mIsRunning = false;
        }
    }

private:
    void run()
    {
        int origId = D2Util::getRightSkillId();

        trace("%s: Start skill %d, orig %d\n", __FUNCTION__, mSkill.skillId, origId);
        DefSubclassProc(origD2Hwnd, WM_KEYDOWN, mSkill.key, 0);
        DefSubclassProc(origD2Hwnd, WM_KEYUP, mSkill.key, 0);
        if (origId == mSkill.skillId) {
            fcDbg(L"no change required orig %d, cur %d", origId, mSkill.skillId);
            return sendMouseDown();
        }
        fcDbg(L"Set Skill to %d cur %d", mSkill.skillId, origId);
        D2Util::setSkill(mSkill.skillId);
        mRetryCount = 0;
        next(&RunActor::checkSkill, 5);
    }

    void checkSkill()
    {
        if (!isFastCastAble()) {
            return done();
        }

        int skillId = D2Util::getRightSkillId();

        trace("%s: wait skill %d, cur %d\n", __FUNCTION__, mSkill.skillId, skillId);
        if (skillId != mSkill.skillId) {
            mRetryCount += 1;
            if (mRetryCount >= 50) { // 250ms
                log_verbose("Failed to set skill id to %d, cur %d\n", mSkill.skillId, skillId);
                return done();
            }
            return next(&RunActor::checkSkill, 5);
        }
        fcDbg(L"Skill %d set", mSkill.skillId);
        trace("%s: wait skill %d done\n", __FUNCTION__, mSkill.skillId);
        //next(&RunActor::sendMouseDown, 0);
        sendMouseDown();
    }

    void sendMouseDown()
    {
        POINT screenPos = { MOUSE_POS->x, MOUSE_POS->y};
        mCurrentPos = screenPos;
        //D2Util::screenToAutoMap(&screenPos, &mCurrentPos);
        trace("%s: send mouse event skill %d cur %d\n", __FUNCTION__,
              mSkill.skillId, D2Util::getRightSkillId());
        mySendMessage(origD2Hwnd, WM_RBUTTONDOWN, MK_RBUTTON,
                      ((DWORD)mCurrentPos.x) | (((DWORD)mCurrentPos.y) << 16));
        mySendMessage(origD2Hwnd, WM_RBUTTONUP, MK_RBUTTON,
                      ((DWORD)mCurrentPos.x) | (((DWORD)mCurrentPos.y) << 16));
        next(&RunActor::done, 10);
    }

    void sendMouseUp()
    {
        mySendMessage(origD2Hwnd, WM_RBUTTONUP, MK_RBUTTON,
                      ((DWORD)mCurrentPos.x) | (((DWORD)mCurrentPos.y) << 16));
        next(&RunActor::done, 5);
        trace("%s: skill %d mouse sent\n", __FUNCTION__, mSkill.skillId);
    }

    void done()
    {
        mIsRunning = false;
        mWatcher->signal(this);
    }

private:
    Watcher *   mWatcher;
    bool        mIsRunning;
    SkillTime   mSkill;
    int         mRetryCount;
    POINT       mCurrentPos;
};

class FastCastActor: public Watcher {
    enum State {
        Idle,
        Skill,
        Restore,
        WaitRestore,
    };

    enum Result {
        Continue,
        Wait,
    };

public:
    static FastCastActor& inst()
    {
        static FastCastActor gFastCastActor;
        return gFastCastActor;
    }

    FastCastActor(): mState(Idle), mCrankCount(0), mIsRunning(false), mActor(this),
                     mOrigSkillId(-1), mWaitRestoreStart(0)
    {
        auto f = [this](Event::Type) { stop(); };
        Event::add(Event::GameStart, f);
        Event::add(Event::GameEnd, f);

        mRestoreDelayTimer.cb = [this](Timer *) {
            crank();
        };
    }

    void  stop()
    {
        mActor.cancel();

        mState = Idle;
        mCrankCount = 0;
        mPendingSkills.clear();
        mIsRunning = false;
        mOrigSkillId = -1;
        mWaitRestoreStart = 0;
        mRestoreDelayTimer.stop();
    }



    void startSkill(const SkillTime& newSkill)
    {
        for (auto& sk: mPendingSkills) {
            if (sk.skillId == newSkill.skillId) {
                sk.startTime = newSkill.startTime;
                return;
            }
        }
        mPendingSkills.push_back(newSkill);

        crank();
    }


private:
    virtual void signal(Actor * actor) override
    {
        mIsRunning = false;
        crank();
    }

    void crank()
    {
        int cur = mCrankCount;

        mCrankCount += 1;
        if (cur != 0) {
            return;
        }

        for (; mCrankCount > 0; mCrankCount -= 1) {
            trace("Start crank cur %d, count %d\n", cur, mCrankCount);
            doCrank();
        }
    }

    void doCrank()
    {
        Result res = Continue;

        while (res != Wait) {
            switch (mState) {
            case Idle:
                res = handleIdle();
                break;
            case Skill:
                res = handleSkill();
                break;
            case Restore:
                res = handleRestore();
                break;
            case WaitRestore:
                res = handleWaitRestore();
                break;
            default:
                log_trace("Unexpected state %d\n", mState);
                mState = Idle;
                return;
            }
        }
    }

    Result handleIdle()
    {
        trace("IDLE: empty %d\n", mPendingSkills.empty());
        if (mPendingSkills.empty()) {
            return Wait;
        }
        mState = Skill;
        return Continue;
    }

    Result handleSkill()
    {
        trace("SKILL: running %d empty %d\n", mIsRunning, mPendingSkills.empty());
        if (mIsRunning) {
            return Wait;
        }
        if (mPendingSkills.empty()) {
            mState = Restore;
            return Continue;
        }

        auto st = mPendingSkills.front();
        mPendingSkills.pop_front();
        runSkill(st);
        return Wait;
    }

    Result handleRestore()
    {
        trace("Restore: orig %d\n", mOrigSkillId);
        if (!mPendingSkills.empty()) {
            mState = Skill;
            return Continue;
        }
        if (mOrigSkillId == -1) {
            mState = Idle;
            return Continue;
        }
        mState = WaitRestore;
        return Continue;
    }

    Result handleWaitRestore()
    {
        trace("WaitRestore: orig %d\n", mOrigSkillId);
        if (!mPendingSkills.empty()) {
            finishWaitRestore();
            mState = Skill;
            return Continue;
        }

        if (mOrigSkillId == -1) {
            finishWaitRestore();
            mState = Idle;
            return Continue;
        }

        if (D2Util::getWeaponSwitch() != mCurrentSwitch) {
            fcDbg(L"Switch weapon, finish restore");
            mOrigSkillId = -1;
            mState = Idle;
            finishWaitRestore();
            return Continue;
        }

        if (mWaitRestoreStart == 0) {
            mWaitRestoreStart = GetTickCount64();
            mRestoreDelayTimer.start(200);
            return Wait;
        }

        if (mRestoreDelayTimer.isPending()) {
            return Wait;
        }

        if (D2Util::getRightSkillId() != mOrigSkillId) {
            // Retry
            trace("WaitRestore: set kill to %d cur %d\n", mOrigSkillId, D2Util::getRightSkillId());
            D2Util::setSkill(mOrigSkillId);
            mWaitRestoreStart = 0;
            return Continue;
        }

        // User is selecting a new skill or switching weapon
        if (D2Util::uiIsSet(UIVAR_CURRSKILL)) {
            mOrigSkillId = -1;
            mState = Idle;
            finishWaitRestore();
            return Continue;
        }

        // Fix SK triggered by hackmap
        // This possible happens in 3 seconds.
        // Still some very corner case that fails to restore, but good enough now.
        uint64_t now = GetTickCount64();
        if (now < mWaitRestoreStart + 30000) {
            mRestoreDelayTimer.start(10);
            return Wait;
        }


        mOrigSkillId = -1;
        mState = Idle;
        finishWaitRestore();
        return Continue;
    }

    void finishWaitRestore()
    {
        mWaitRestoreStart = 0;
        mRestoreDelayTimer.stop();
        mIsRunning = false;
    }

    void runSkill(const SkillTime& st)
    {
        if (mOrigSkillId == -1) {
            mOrigSkillId = D2Util::getRightSkillId();
            mCurrentSwitch = D2Util::getWeaponSwitch();
        }
        mIsRunning = true;
        mActor.startSkill(st);
    }


    bool isRunning() const
    {
        return mIsRunning != 0;
    }

private:
    State               mState;
    int                 mCrankCount;

    std::list<SkillTime> mPendingSkills;
    bool                mIsRunning;
    RunActor            mActor;

    int                 mOrigSkillId;
    BYTE                mCurrentSwitch;
    uint64_t            mWaitRestoreStart;
    Timer               mRestoreDelayTimer;
};

static bool doFastCast(BYTE key, BYTE repeat)
{
    if (!isFastCastAble()) {
        D2Util::showVerbose(L"Not in Game: %d, repeat %d", D2CheckUiStatus(0), repeat);
        return false;
    }
    fcDbg(L"key: %d, repeat %d, swap 0x%x", key, repeat, WEAPON_SWITCH);
    int         func = D2Util::getKeyFunc(key);

    if (func < 0) {
        return false;
    }

    int         skillId = D2Util::getSkillId(func);
    if (skillId < 0) {
        fcDbg(L"KEY %u -> %u (unbind)", key, func);
        return false;
    }

    trace("KEY %u -> %u -> skill %u\n", key, func, skillId);

    FastCastActor::inst().startSkill({key, skillId, GetTickCount64()});

    return true;
}

static bool fastCastToggle(struct HotkeyConfig * config)
{
    if (fastCastEnabled) {
        FastCastActor::inst().stop();
        fastCastEnabled = false;
    } else {
        fastCastEnabled = true;
    }
    log_verbose("FastCast: toggle: %d\n", fastCastEnabled);
    D2Util::showInfo(L"����ʩ����%s", fastCastEnabled? L"����" : L"����");

    if ((config->hotKey & (Hotkey::Ctrl | Hotkey::Alt)) != 0) {
        return true;
    }

    return false;
}

static void fastCastLoadConfig()
{
    auto section = CfgLoad::section("helper.fastcast");

    fastCastEnabled = section.loadBool("enable", true);
    auto keyString = section.loadString("toggleKey");
    fastCastToggleKey = Hotkey::parseKey(keyString);

    if (fastCastToggleKey != Hotkey::Invalid) {
        log_trace("FastCast: toggle key %s -> 0x%llx\n", keyString.c_str(), (unsigned long long)fastCastToggleKey);
    }
    log_verbose("FastCast: enable: %d\n", fastCastEnabled);
}

static int fastCastModuleInstall()
{
    fastCastLoadConfig();
    keyRegisterHandler(doFastCast);
    FastCastActor::inst();

    if (fastCastToggleKey != Hotkey::Invalid) {
        keyRegisterHotkey(fastCastToggleKey, fastCastToggle, nullptr);
    }

    return 0;
}

static void fastCastModuleUninstall()
{
}

static void fastCastModuleOnLoad()
{
}

static void fastCastModuleReload()
{
}


D2HModule fastCastModule = {
    "Fast Cast",
    fastCastModuleInstall,
    fastCastModuleUninstall,
    fastCastModuleOnLoad,
    fastCastModuleReload,
};