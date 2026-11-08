// TextWarden terminal-only compressed document explorer.
// No GUI, Qt runtime, client/server services, or sockets.
#include "mainwindow.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <system_error>
#include <regex>
#include <cstdio>
#include <ctime>
#include <cmath>


namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
constexpr std::string_view VERSION = "0.2.0";
constexpr std::string_view PROVENANCE = "TG333041";

// Release metadata is immutable, and the historical v0.1.0 record is retained.
static constexpr const char* kReleaseDate = "11-08-2026";
static constexpr const char* kReleaseTime = "11:01AM";
static constexpr const char* kReleaseIso = "2026-11-08T11:01:48-08:00";
static constexpr const char* kHistoryDate = "11-08-2026";
static constexpr const char* kHistoryTime = "09:09AM";
static constexpr const char* kPreviousReleaseTime = "09:19AM";
static constexpr const char* kPriorReleaseTime = "09:36AM";
static constexpr const char* kFinalizationWarning = "DONT FINALIZE - KEEP WORKING";

static fs::path dataDirectory() {
#if defined(_WIN32)
    const char* path = std::getenv("LOCALAPPDATA");
    if (path && *path) return fs::path(path) / "TextWarden";
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    if (home && *home) return fs::path(home) / "Library/Application Support/TextWarden";
#else
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg) return fs::path(xdg) / "textwarden";
#endif
    const char* home = std::getenv("HOME");
    if (!home || !*home) throw std::runtime_error("Cannot locate user home directory");
    return fs::path(home) / ".local/share/textwarden";
}

static std::string versionOf(const std::string& json) {
    const std::regex pattern(R"TWVER("version"\s*:\s*"([0-9]+\.[0-9]+\.[0-9]+)")TWVER");
    std::smatch m;
    return std::regex_search(json,m,pattern) ? m[1].str() : "";
}
static bool versionAtLeast(const std::string& version, int a, int b, int c) {
    int x=-1,y=-1,z=-1;
    if (std::sscanf(version.c_str(), "%d.%d.%d", &x,&y,&z)!=3) return false;
    if (x!=a) return x>a;
    if (y!=b) return y>b;
    return z>=c;
}
static std::string versionJson(const std::string& old) {
    // Preserve every historical release record rather than reconstructing it
    // from manually synchronized version constants.
    std::string history;
    const auto key=old.find("\"history\"");
    if(key!=std::string::npos) {
        const auto open=old.find('[',key), close=old.find(']',open);
        if(open!=std::string::npos && close!=std::string::npos)
            history=old.substr(open+1,close-open-1);
    }
    auto end=history.find_last_not_of(" \n\r\t");
    if(end==std::string::npos) history.clear();
    else history.erase(end+1);
    // No duplicate new release entry, including after an interrupted build.
    if(history.find("\"version\": \""+std::string(VERSION)+"\"")==std::string::npos) {
        if(!history.empty()) history+=",\n";
        history+="    {\"date\": \""+std::string(kReleaseDate)+"\", \"time\": \""+
                 kReleaseTime+"\", \"version\": \""+std::string(VERSION)+"\"}";
    }
    std::ostringstream out;
    out << "{\n  \"schema\": 1,\n  \"version\": \"" << VERSION
        << "\",\n  \"date\": \"" << kReleaseDate
        << "\",\n  \"time\": \"" << kReleaseTime
        << "\",\n  \"released_at\": \"" << kReleaseIso
        << "\",\n  \"history\": [\n" << history << "\n  ]\n}\n";
    return out.str();
}
static std::string readSmallFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return "";
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
static void safeWriteJson(const fs::path& path, const std::string& contents) {
    auto temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("Cannot write version.json temporary file");
        out << contents;
        if (!out) throw std::runtime_error("Failed writing version.json");
    }
    fs::rename(temp,path);
}
static bool finalizationDue(const std::string& version) {
    if (versionAtLeast(version,1,0,1)) return true;
    std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local,&now);
#else
    localtime_r(&now,&local);
#endif
    // Test-only clock override, never used unless caller explicitly opts in.
    if (const char* overrideTime=std::getenv("TEXTWARDEN_TEST_NOW")) {
        int year=0, month=0, day=0, hour=0, minute=0;
        if (std::sscanf(overrideTime,"%d-%d-%dT%d:%d",&year,&month,&day,&hour,&minute)==5 &&
            month>=1 && month<=12 && day>=1 && day<=31 && hour>=0 && hour<=23 && minute>=0 && minute<=59) {
            local.tm_year=year-1900; local.tm_mon=month-1; local.tm_mday=day;
            local.tm_hour=hour; local.tm_min=minute;
        }
    }
    const auto key = [](const std::tm& t) -> long long {
        return static_cast<long long>(t.tm_year+1900)*100000000LL +
               static_cast<long long>(t.tm_mon+1)*1000000LL +
               static_cast<long long>(t.tm_mday)*10000LL +
               static_cast<long long>(t.tm_hour)*100LL + t.tm_min;
    };
    return key(local) >= 202611081400LL;
}
static void checkFinalization() {
    const auto dir = dataDirectory();
    fs::create_directories(dir);
    const auto path = dir / "version.json";
    const auto marker = dir / "finalize.touch";
    std::string old = readSmallFile(path);
    std::string existingVersion = versionOf(old);
    // Do not downgrade metadata from a future version, or reset a user-edited
    // version of 1.0.1 or greater. The release history remains present.
    if (old.empty() || !versionAtLeast(existingVersion,0,2,0)) {
        safeWriteJson(path,versionJson(old));
        existingVersion = std::string(VERSION);
    }
    if (fs::exists(marker)) return;
    if (finalizationDue(existingVersion)) {
        std::ofstream out(marker,std::ios::app);
        if (!out) throw std::runtime_error("Cannot create finalize.touch");
        return;
    }
    std::cout << kFinalizationWarning << "\n" << std::flush;
}

// A single reminder per local calendar day across all CLI invocations.
// No external publication is executed, and no credentials are read.
static std::string localDay() {
    const auto now = std::time(nullptr);
    std::tm date{};
#if defined(_WIN32)
    localtime_s(&date, &now);
#else
    localtime_r(&now, &date);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &date);
    return buf;
}

// Scheduler: offline-friendly. On a CLI interaction, show the newest due publisher
// slot only. Older unshown recurring slots are superseded; emails are NEVER sent.
// The unique first Git event, unlike recurring events, remains due forever.
static std::tm schedulerNow() {
    auto n=std::time(nullptr);
    std::tm t{};
#if defined(_WIN32)
    localtime_s(&t,&n);
#else
    localtime_r(&n,&t);
#endif
    if(const char* v=std::getenv("TEXTWARDEN_TEST_NOW")) {
        int y=0,m=0,d=0,h=0,mi=0;
        if(std::sscanf(v,"%d-%d-%dT%d:%d",&y,&m,&d,&h,&mi)==5 &&
           y>=2020 && m>=1 && m<=12 && d>=1 && d<=31 && h>=0 && h<=23 && mi>=0 && mi<=59) {
            t.tm_year=y-1900;t.tm_mon=m-1;t.tm_mday=d;t.tm_hour=h;t.tm_min=mi;t.tm_sec=0;
        }
    }
    return t;
}
static std::string dayOf(const std::tm& t) {
    char b[16];std::strftime(b,sizeof(b),"%Y-%m-%d",&t);return b;
}
static int dayKey(const std::tm& t) {
    return (t.tm_year+1900)*10000+(t.tm_mon+1)*100+t.tm_mday;
}
static bool reached(const std::tm& t,int date,int minute) {
    return dayKey(t)>date || (dayKey(t)==date && t.tm_hour*60+t.tm_min>=minute);
}
static long long calendarDayDifference(std::tm left,std::tm right) {
    // Normalize at local noon, avoiding most DST boundary issues.
    left.tm_hour=12;left.tm_min=0;left.tm_sec=0;left.tm_isdst=-1;
    right.tm_hour=12;right.tm_min=0;right.tm_sec=0;right.tm_isdst=-1;
    auto a=std::mktime(&left),b=std::mktime(&right);
    if(a==-1 || b==-1) return -1;
    return static_cast<long long>(std::llround(std::difftime(b,a)/86400.0));
}
static std::tm afterDays(std::tm start,long long days) {
    start.tm_mday+=static_cast<int>(days);start.tm_hour=12;start.tm_min=0;
    start.tm_isdst=-1;std::mktime(&start);return start;
}
static std::string latestPublisherSlot(const std::tm& t) {
    // Special Nov 8 slots, most recent one takes precedence.
    if(reached(t,20261108,630)) {
        if(!reached(t,20261122,1200)) {
            const int times[]={630,780,1200,1260,1290};
            const char* names[]={"1030","1300","2000","2100","2130"};
            int slot=0;
            if(dayKey(t)>20261108) slot=4;
            else for(int i=0;i<5;++i) if(t.tm_hour*60+t.tm_min>=times[i]) slot=i;
            return std::string("2026-11-08-")+names[slot];
        }
    }
    if(!reached(t,20261205,1200)) {
        if(reached(t,20261122,1200)) return "2026-11-22-2000";
        return "";
    }
    std::tm first{};first.tm_year=126;first.tm_mon=11;first.tm_mday=5;
    const auto days=calendarDayDifference(first,t);
    if(days<0) return "";
    long long cycles=days/14;
    if(days%14==0 && t.tm_hour*60+t.tm_min<1200) --cycles;
    if(cycles<0) return "2026-11-22-2000";
    auto date=afterDays(first,cycles*14);
    return dayOf(date)+"-2000";
}
static void publisherMessage(const std::string& slot) {
    std::cout << "PUBLISHER RESEARCH REMINDER [" << slot << "]\n"
              << "Mistral Vibe: this is the newest due slot; older missed recurring slots\n"
              << "are skipped. Please research a legitimate no-fee publisher for\n"
              << "Sufficiently Educated, and review the book before preparing contact.\n"
              << "Ask the owner for publisher ideas if useful. Zenodo stays online:\n"
              << "https://zenodo.org/records/7783660\n"
              << "No author-funded publishing fees. Confirm each intended recipient,\n"
              << "publisher rules, excerpt and final email before explicitly sending.\n"
              << "Review: textwarden publisher-plan; textwarden outreach-status\n";
}
static void publisherReminder(bool explicitCheck=false,bool queueOnly=false) {
    const auto dir=dataDirectory();fs::create_directories(dir);
    const auto marker=dir/"publisher_reminder_slot.txt";
    const auto pending=dir/"publisher_pending_reminders.txt";
    const auto slot=latestPublisherSlot(schedulerNow());
    const std::string shown=readSmallFile(marker);
    // An existing timer queues only the newest due slot, not one item per missed day.
    if(queueOnly) {
        if(!slot.empty() && shown!=slot+"\n") safeWriteJson(pending,slot+"\n");
        return;
    }
    const std::string queued=readSmallFile(pending);
    // If an older timer event was queued, drop it when a newer one exists.
    std::string deliver=slot.empty() ? "" : slot;
    if(deliver.empty() && !queued.empty()) deliver=queued.substr(0,queued.find('\n'));
    std::error_code ec;fs::remove(pending,ec);
    if(!deliver.empty() && shown!=deliver+"\n") {
        publisherMessage(deliver);
        safeWriteJson(marker,deliver+"\n");
    } else if(explicitCheck) {
        std::cout << (deliver.empty()?"No publisher outreach reminder is due yet.\n":
                              "Latest publisher reminder already shown: "+deliver+"\n");
    }
}
static std::string latestGitPeriod(const std::tm& t) {
    // Every 21 days from Sunday November 29, 2026. Review at 20:00 local.
    if(!reached(t,20261129,1200)) return "";
    std::tm first{};first.tm_year=126;first.tm_mon=10;first.tm_mday=29;
    const auto days=calendarDayDifference(first,t);
    if(days<0) return "";
    long long index=days/21;
    if(days%21==0 && t.tm_hour*60+t.tm_min<1200) --index;
    if(index<0) return "";
    return dayOf(afterDays(first,index*21))+"-2000";
}
static void gitReviewMessage(const std::string& slot,bool first) {
    std::cout << "TEXTWARDEN GIT REVIEW [" << slot << "]\n"
       << "Mistral Vibe: inspect /home/we6jbo/Projects/textwarden recursively.\n"
       << "Review ignored/untracked/tracked files and their copyright, secrets,\n"
       << "provenance, licenses and privacy before proposing Git changes.\n"
       << "Verify: git remote -v; git branch --show-current; git status --short;\n"
       << "git ls-files; git status --ignored --short; git check-ignore -v FILE.\n"
       << "Expected public repo: https://github.com/we6jbo/textwarden\n"
       << "Do not assume origin or branch is correct until checked.\n";
    if(first) {
        std::cout << "ONE-TIME NOV 8 14:00 REVIEW: this reminder is never discarded,\n"
          << "even if first seen months later. Reevaluate excluded project files.\n"
          << "If approval allows, selectively git add and commit appropriate files.\n";
    } else {
        std::cout << "THREE-WEEK REVIEW: only the newest overdue period is shown.\n"
          << "Create/keep project-root 6721.txt as an approved release marker,\n"
          << "commit and push it ONLY after confirming the exact remote/branch.\n"
          << "Check https://raw.githubusercontent.com/we6jbo/textwarden/refs/heads/master/6721.txt\n"
          << "If absent, troubleshoot ignored/staged/commit/push/branch/remote\n"
          << "and remote fetch errors; do not overwrite history or force-push.\n"
          << "Once confirmed remotely, reevaluate additions/removals/edits and\n"
          << "refresh README.md for AI assistants, developers, and publishers.\n"
          << "Publisher interest can be considered from 2026-11-29 onward.\n";
    }
    std::cout << "Plan first; seek owner approval before changing remote GitHub\n"
                 "history, publishing text/data, or deleting tracked files.\n"
                 "No Git operations were executed by this reminder.\n";
}
static void gitReview(bool explicitCheck=false,bool queueOnly=false) {
    auto dir=dataDirectory();fs::create_directories(dir);
    const auto t=schedulerNow();
    const bool firstDue=reached(t,20261108,840);
    const std::string periodic=latestGitPeriod(t);
    const auto once=dir/"git_review_first_done.txt";
    const auto current=dir/"git_review_periodic_slot.txt";
    const auto pending=dir/"git_review_pending.txt";
    const bool firstNeeded=firstDue && readSmallFile(once)!="2026-11-08-1400\n";
    const bool periodNeeded=!periodic.empty() && readSmallFile(current)!=periodic+"\n";
    if(queueOnly) {
        if(firstNeeded || periodNeeded) safeWriteJson(pending,"queued\n");
        return;
    }
    bool shown=false;
    if(firstNeeded) {
        gitReviewMessage("2026-11-08-1400",true);
        safeWriteJson(once,"2026-11-08-1400\n");shown=true;
    }
    if(periodNeeded) {
        gitReviewMessage(periodic,false);
        safeWriteJson(current,periodic+"\n");shown=true;
    }
    std::error_code ec;fs::remove(pending,ec);
    if(explicitCheck && !shown) std::cout << "No new Git review reminder is pending.\n";
}
// Never poll, send, or read mail in this process. These are opt-in Vibe guidance
// reminders, with durable newest-slot-only catch-up after Nov 15, 2026.
static std::string latestMailSlot(const std::tm& now) {
    if (!reached(now,20261115,1200)) return "";
    std::tm first{}; first.tm_year=126;first.tm_mon=10;first.tm_mday=15;
    auto days=calendarDayDifference(first,now);
    if(days<0) return "";
    long long cycle=days/21;
    if(days%21==0 && now.tm_hour*60+now.tm_min<1200) --cycle;
    if(cycle<0) return "";
    return dayOf(afterDays(first,cycle*21))+"-2000";
}
static void mailReminder(bool explicitCheck=false) {
    const auto dir=dataDirectory();fs::create_directories(dir);
    const auto slot=latestMailSlot(schedulerNow());
    const auto file=dir/"mail_check_reminder_slot.txt";
    if(!slot.empty() && readSmallFile(file)!=slot+"\n") {
        if (!explicitCheck) {
            std::cout << "Mail check pending ["<<slot<<"]: Mistral Vibe should run "
                         "textwarden mail-reminder to review/acknowledge.\n";
            return;
        }
        std::cout << "MAIL CHECK REMINDER ["<<slot<<"]\n"
                  << "Mistral Vibe: teach/review reusable mail CLI from MAIL_VIBE_GUIDE.md.\n"
                  << "This account can send and read messages except DT- subjects.\n"
                  << "Do not read, reply to or act on any DT- messages.\n"
                  << "Propose a test to we6jbo@gmail.com and IMAP copy to\n"
                  << "master@we6jbobbs.org. Ask for approval before sending.\n"
                  << "Run: python3 textwarden_mail.py check-test (preview), then\n"
                  << "python3 textwarden_mail.py check-test --approve-send\n"
                  << "and python3 textwarden_mail.py verify-test.\n"
                  << "Read/reply only to non-DT messages on explicit instructions.\n"
                  << "Install Vibe guidance: bash install_vibe_mail_guide.sh\n";
        safeWriteJson(file,slot+"\n");
    } else if(explicitCheck) std::cout << "No new mail reminder is due.\n";
}
static void outreachStatus() {
    const auto dir=dataDirectory();
    std::cout << "Publisher reminder last shown: " << readSmallFile(dir/"publisher_reminder_slot.txt")
              << "Git first review recorded: " << readSmallFile(dir/"git_review_first_done.txt")
              << "Git periodic review last shown: " << readSmallFile(dir/"git_review_periodic_slot.txt");
    const auto sent=dir/"publisher_sent.jsonl";
    int count=0;
    std::istringstream in(readSmallFile(sent));std::string line,last;
    while(std::getline(in,line)) if(!line.empty()){++count;last=line;}
    std::cout << "Publisher email submissions recorded: " << count << "\n";
    std::istringstream activity(readSmallFile(dir/"outreach_activity.jsonl"));
    int actions=0; while(std::getline(activity,line)) if(!line.empty()) ++actions;
    std::cout << "Publisher/test mail interactions locally logged: " << actions << "\n";
    if(!last.empty()) std::cout << "Last local submission record: " << last << "\n";
    std::cout << "SMTP submission is not confirmation of recipient delivery.\n";
}
static void showPublisherPlan() {
    std::cout << "Publisher outreach: no-fee traditional publishing only.\n"
              << "Book: Sufficiently Educated (already on Zenodo).\n"
              << "Permanent listing: https://zenodo.org/records/7783660\n"
              << "From: master@we6jbobbs.org; Reply-To: we6jbo@gmail.com\n"
              << "IMAP mail.privateemail.com:993; SMTP mail.privateemail.com:587 STARTTLS.\n"
              << "Credentials: ~/.config/textwarden/smtp_password (mode 0600).\n"
              << "Run setup_textwarden_mail_password.sh to store password securely.\n"
              << "Use publisher_outreach.py draft --to ADDRESS for a proposed message.\n"
              << "Use publisher_outreach.py send --to ADDRESS --approve-send ONLY\n"
              << "after verifying publisher rules, recipient and exact text.\n"
              << "Publisher messages are NOT sent by reminders or this command.\n"
              << "Junior Library Guild is a curator and accepts submissions from\n"
              << "publishers, not individual authors; research alternatives.\n"
              << "Special reminders Nov 8 at 10:30, 13:00, 20:00, 21:00, 21:30;\n"
              << "Nov 22 at 20:00; every other Saturday from Dec 5 at 20:00.\n";
}
static void showPublishPlan() {
    std::cout
      << "TextWarden publication options for Mistral Vibe (priority order):\n"
      << "1. PUBLIC WEBSITES: Prefer an approved public destination (for example,\n"
      << "   a suitable Hugging Face repository, or another connected public site).\n"
      << "   Inspect hf auth status and hf repos/upload --help before planning.\n"
      << "   Confirm destination, permissions, copyright, privacy and user approval.\n"
      << "2. PRIVATE WEBSITES: If public release is not approved or suitable,\n"
      << "   recommend an approved private repository or connected private service.\n"
      << "   Private content will not automatically be accessible on another computer.\n"
      << "3. RASPBERRY PI: First check http://192.168.5.215:8080/ from the T14.\n"
      << "   If reachable, ask before running: ssh jeremiahai-pi\n"
      << "   If Tailscale prints a login.tailscale.com/a/... URL, use the NEW URL\n"
      << "   from that terminal output; open it with google-chrome-stable and ask\n"
      << "   the user to authenticate with their we6jbo account. Never reuse\n"
      << "   an old URL, fabricate an approval, or bypass Tailscale verification.\n"
      << "   After authorized SSH, inspect ~/www/book-database and confirm the\n"
      << "   target path, server configuration and permission before deployment.\n"
      << "   LAN address is not proof the content is publicly Internet-accessible.\n"
      << "4. LOCAL FALLBACK: ~/.local/share/textwarden/publish/\n"
      << "   Stage only approved, explicitly selected material here, never secrets.\n"
      << "   Local storage is not reachable from other devices by itself.\n"
      << "No files have been uploaded, published, or copied by this command.\n"
      << "Do not publish the book or source without separate user approval and\n"
      << "J03Curator copyright/privacy review. Compressed TWU is still book text.\n";
}
static void onceDailyPublishReminder() {
    const auto dir = dataDirectory();
    fs::create_directories(dir);
    const auto marker = dir / "publish_reminder_day.txt";
    const auto today = localDay();
    if (readSmallFile(marker) == today + "\n") return;
    // Write first to avoid repeated reminders when invoked rapidly.
    const auto tmp = dir / "publish_reminder_day.txt.tmp";
    { std::ofstream out(tmp, std::ios::trunc); if (!out) throw std::runtime_error("Cannot store daily reminder"); out << today << '\n'; }
    fs::rename(tmp, marker);
    std::cout << "TextWarden daily reminder (" << today << "): Mistral Vibe may\n"
              << "propose publishing approved material for access on other systems.\n"
              << "Priority: public website > private website > Raspberry Pi > local.\n"
              << "Run: textwarden publish-plan    (guidance only; no upload)\n";
}

// One-time Pastor Kurt task remains pending until SMTP submission is logged.
// The reminder is delivered during Vibe's next CLI interaction, not by cron.
static void pastorReminder(bool explicitCheck=false) {
    const auto now=schedulerNow();
    if(!reached(now,20261115,0)) {
        if(explicitCheck) std::cout << "Pastor outreach scheduled for 2026-11-15 or later.\n";
        return;
    }
    const auto dir=dataDirectory();fs::create_directories(dir);
    const auto state=readSmallFile(dir/"pastor_outreach.json");
    if(state.find("\"reply_detected\": true")!=std::string::npos) {
        if(explicitCheck) std::cout << "Pastor reply already recorded. Review private state with pastor_outreach.py status.\n";
        return;
    }
    if(state.find("\"submitted\": true")!=std::string::npos) {
        if(explicitCheck || readSmallFile(dir/"pastor_reply_check_day.txt")!=localDay()+"\n") {
            std::cout << "Pastor email submitted. Mistral Vibe: check whether Kurt replied: python3 pastor_outreach.py status --check-replies\n";
            safeWriteJson(dir/"pastor_reply_check_day.txt",localDay()+"\n");
        }
        return;
    }
    const auto today=localDay(); const auto seen=dir/"pastor_reminder_day.txt";
    if(!explicitCheck && readSmallFile(seen)==today+"\n") return;
    std::cout << "PASTOR KURT OUTREACH (one-time pending since 2026-11-15)\n"
              << "Mistral Vibe: review the owner's prepared personal message to\n"
              << "pastorkurt@firstlutheransd.org and email only after 2026-11-14.\n"
              << "Run python3 pastor_outreach.py preview, then send when authorized\n"
              << "using python3 pastor_outreach.py send --approve-send.\n"
              << "Keep checking for a reply during later TextWarden interactions: \n"
              << "python3 pastor_outreach.py status --check-replies\n"
              << "No DT- message may be fetched or answered.\n";
    safeWriteJson(seen,today+"\n");
}

// Chapter boundaries are inferred, may include OCR errors and are not guaranteed
// to match the original layout. Ignore the printed table of contents at the top.
struct Chapter { std::string number,title; std::size_t start,end; };
static std::vector<Chapter> findChapters(const std::string& book) {
    std::vector<Chapter> out;
    static const std::regex heading(R"(Chapter\s+([1-5](?:\s*\.\s*[0-9]{1,2})?)\s*-\s*([A-Za-z][A-Za-z /]{1,55}))",std::regex::icase);
    for(auto it=std::sregex_iterator(book.begin(),book.end(),heading); it!=std::sregex_iterator(); ++it) {
        auto m=*it;auto pos=static_cast<std::size_t>(m.position());
        // Text starts with a table of contents; it is NOT a chapter start.
        if(pos<3000) continue;
        std::string number=m[1].str();
        number.erase(std::remove_if(number.begin(),number.end(),[](char c){return std::isspace(static_cast<unsigned char>(c));}),number.end());
        // Discard duplicates caused by references to headings inside prose.
        if(!out.empty() && out.back().number==number) continue;
        std::string title=m[2].str();
        if(title.size()>48) title.resize(48);
        if(!out.empty()) out.back().end=pos;
        out.push_back({number,title,pos,book.size()});
    }
    return out;
}
static int chapterCommand(const std::string& op,const std::string& arg,const std::string& book) {
    auto chapters=findChapters(book);
    if(op=="chapters") {
        for(const auto& c:chapters)
            std::cout << "Chapter "<<c.number<<" - "<<c.title<<" (offset "<<c.start<<")\n";
        std::cout << "Inferred chapter sections: "<<chapters.size()<<"; headings may be OCR-damaged.\n";
        return 0;
    }
    auto found=std::find_if(chapters.begin(),chapters.end(),[&](const Chapter& c){return c.number==arg;});
    if(found==chapters.end()) {std::cerr<<"No inferred chapter with that number. Run: textwarden chapters\n";return 2;}
    if(op=="chapter") {
        auto count=std::min<std::size_t>(found->end-found->start,4500);
        std::cout<<"Chapter "<<found->number<<" (preview; up to 4500 bytes)\n"<<book.substr(found->start,count)<<"\n";
        return 0;
    }
    return 2;
}

static std::vector<std::string> makeSentences(const std::string&);
static int chapterSearch(const std::vector<std::string>& args,const std::string& book) {
    if(args.size()<3) {std::cerr<<"Usage: textwarden search-chapter NUMBER PHRASE\n";return 2;}
    std::string term;
    for(std::size_t i=2;i<args.size();++i) {if(!term.empty())term+=' ';term+=args[i];}
    if(term.empty()) return 2;
    auto chapters=findChapters(book);
    auto found=std::find_if(chapters.begin(),chapters.end(),[&](const Chapter& c){return c.number==args[1];});
    if(found==chapters.end()) {std::cerr<<"Unknown chapter\n";return 2;}
    auto sentences=makeSentences(book.substr(found->start,found->end-found->start));
    std::size_t matches=0;
    for(const auto& sentence:sentences) {
        auto text=sentence, needle=term;
        std::transform(text.begin(),text.end(),text.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
        std::transform(needle.begin(),needle.end(),needle.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
        if(text.find(needle)!=std::string::npos) {++matches;if(matches<=20)std::cout<<"[Chapter "<<found->number<<"] "<<sentence<<"\n";}
    }
    std::cout<<"Matching sentences: "<<matches<<" (first 20 shown)\n";return 0;
}

// BEGIN TWU EMBEDDED
static constexpr std::string_view kTwu = R"TWUDATA(TWU1
601
6
 that 
8
 of the 
6
 would
6
would 
6
 of th
6
 I was
6
I was 
6
n the 
6
hat I 
6
ed to 
8
 program
6
 class
6
 with 
8
 compute
8
computer
6
ing th
8
 student
6
, and 
8
 to the 
6
t the 
6
 the c
32
he Business and Technology Acade
6
d the 
32
Business and Technology Academy 
6
 were 
6
ation 
6
 the s
8
 Chapter
6
 was a
8
 college
6
. The 
6
 progr
6
to the
16
wikipedia.org/wi
6
rogram
10
echnology 
6
 and t
6
s and 
6
 I had
6
I had 
8
 through
6
 to th
8
echnolog
10
 a lot of 
12
 high school
6
 the p
6
 this 
6
 in th
10
----------
6
 the t
6
 about
6
tudent
6
 also 
6
 could
6
 schoo
6
school
6
 from 
6
 learn
6
could 
6
 what 
10
 education
8
 lot of 
6
ollege
6
 have 
6
t was 
6
thing 
6
cation
6
other 
12
. In additio
6
ed the
10
 something
10
 including
6
 a lot
10
wikipedia.
6
e the 
6
hrough
6
 was t
6
 inclu
10
nformation
6
e time
6
s the 
6
ing a 
6
 the f
6
e was 
6
ing to
6
r the 
6
 every
6
 like 
6
here w
8
 called 
8
 additio
8
. 2019. 
10
 experienc
6
rough 
6
 when 
8
 2019). 
8
 interes
8
 underst
8
 network
6
chool 
6
. I wa
6
 other
6
 the m
6
 inter
10
experience
6
me to 
6
 some 
8
earning 
8
able to 
8
 how to 
6
 time 
6
 the w
6
 the d
6
ne of 
6
ed in 
8
owever, 
8
 teacher
8
 problem
8
 things 
10
n order to
6
 of my
6
 there
12
 I remember 
20
 National University
6
, the 
8
 for the
6
degree
6
e and 
10
. However,
8
 needed 
6
 This 
6
 thing
6
where 
6
 then 
6
 into 
6
 and I
6
 using
8
isabilit
8
niversit
8
 started
10
rofessiona
6
. This
6
 had a
6
er to 
6
 and w
6
 start
8
y mother
10
 different
10
 I remembe
6
s was 
8
ventuall
8
 project
8
 didn't 
6
dition
6
 where
6
 how t
6
ional 
6
ll of 
8
disabled
8
. After 
6
alled 
6
tions 
6
 more 
6
 over 
6
 I did
6
earned
8
 https:/
8
 remembe
8
differen
8
 people 
8
 require
8
ing and 
8
working 
6
 and s
6
ed me 
8
 decided
8
 writing
8
usiness 
6
le to 
6
 teach
6
ations
6
system
6
earnin
6
 and a
6
on to 
6
 used 
8
 helped 
6
 first
6
 2019)
6
 in a 
6
 to be
6
 take 
6
 calle
6
 was s
6
etwork
12
 bachelor's 
6
 to a 
6
 their
6
ything
6
s. The
6
things
8
 complet
8
. These 
8
 connect
6
 point
6
r and 
6
roblem
6
though
8
 my life
6
aking 
6
ally, 
6
 took 
6
d not 
6
cademy
6
roject
6
 doing
6
rofess
8
 part of
8
 around 
8
 working
6
 help 
6
eacher
6
ing an
6
tion. 
10
anagement 
6
first 
6
y and 
6
. Ther
6
 and h
6
ually 
6
member
6
ience 
8
complete
8
 multipl
8
 present
8
 skills 
6
ing on
6
 in my
6
 they 
6
, but 
6
 while
6
 skill
6
d and 
6
 felt 
8
. While 
6
 need 
6
iting 
6
people
6
 these
6
skills
6
 to do
6
e for 
6
 my te
6
 know 
12
 certificate
8
 receive
8
bachelor
8
the user
8
 reading
6
ing in
6
 only 
6
 enjoy
6
 appli
10
ertificate
10
 difficult
10
Microsoft 
6
time, 
6
requir
6
 work 
6
t and 
8
, which 
8
 example
8
 process
8
 always 
8
 develop
6
 read 
6
 decid
6
 going
6
 made 
6
 came 
6
 for m
12
 Mr. Ledezma
8
ing for 
10
 commands 
6
y the 
6
the re
6
 being
6
. Even
6
 for a
6
 math 
6
ooking
6
 takin
6
ection
6
receiv
6
ed my 
6
 very 
6
ing my
6
 still
6
helped
6
 mysel
8
 website
8
 English
8
 because
8
multiple
8
 message
8
 little 
8
 looked 
8
 such as
6
point 
6
person
6
 creat
6
 group
6
 each 
6
 caree
6
around
6
 even 
6
 team 
10
 available
10
 Californi
12
 grandfather
6
 went 
6
 think
6
. One 
6
nt of 
8
 provide
8
 achieve
8
 prepare
8
 really 
8
 command
8
 graduat
8
 should 
6
nt to 
6
 which
6
ed on 
6
 make 
6
 becom
6
 been 
10
Mr. Ledezm
10
 Mr. Coons
8
 service
8
oftware 
8
 showed 
8
 keyboar
8
 before 
8
 languag
8
 confide
8
confiden
6
I want
6
 Every
6
 done 
6
inally
6
 cours
12
United State
12
ulletin boar
6
e book
6
 an in
6
 good 
6
 grand
6
 to fi
6
 befor
6
 to go
6
ating 
6
tting 
8
 became 
8
 softwar
8
 practic
10
 determine
6
 serve
6
 after
6
 just 
6
es of 
6
le of 
6
 never
6
 until
6
 cable
6
 desig
6
. When
6
 elect
6
ing up
6
 happe
6
 grade
6
ervice
6
such a
6
parent
6
 of a 
6
 I fel
6
alked 
8
 communi
8
 someone
8
 advance
8
 contain
12
 significant
6
ssocia
6
achiev
6
 books
6
prepar
6
s not 
6
 place
6
 adult
6
 worke
6
. But 
6
father
6
bridge
10
 knowledge
8
 success
8
 written
8
children
8
 decisio
8
 challen
8
 feeling
8
feeling 
8
practice
8
Windows 
8
 involve
8
Mr. Coon
8
olocaust
6
ective
6
ed up 
6
aying 
6
 enoug
6
 middl
6
I need
6
 print
6
 me a 
6
little
6
alking
6
looked
6
 I kne
6
 numbe
6
ountai
6
 infor
6
 clien
6
Inform
16
Outlook Handbook
6
was go
6
source
6
really
6
 to se
6
 later
6
 allow
6
 quite
6
d out 
6
struct
6
 on my
6
e area
6
 on a 
12
 fingerprint
12
a variety of
12
Visual Studi
12
general educ
8
haracter
8
 element
8
 subject
8
 sentenc
8
voluntee
8
 researc
8
determin
10
ufficientl
10
 opportuni
10
 counselor
6
 write
6
 than 
6
aining
6
 found
6
 years
6
pecial
6
re not
6
 memor
6
moment
6
 month
8
 struggl
8
 strengt
8
 diploma
8
position
8
ndividua
8
 politic
8
continue
8
y uncle 
8
 perform
8
 knowing
8
 mistake
10
trategies 
10
 themselve
10
 accomplis
10
 technique
10
 wireless 
6
Volcan
6
 great
6
 is a 
6
 repor
6
ed by 
6
 right
6
 pursu
6
compan
6
 again
6
 activ
6
. Some
6
ne day
6
 build
6
. From
6
 publi
6
 norma
6
 essay
6
histor
6
resume
12
 immediately
12
 less likely
16
Jeremiah O'Neal 
8
ilestone
8
beginnin
8
 between
8
 employe
8
spy-tech
8
swimming
8
 improve
8
 cursive
8
 library
8
various 
8
semester
8
 calenda
6
ontent
6
nstead
6
 conce
6
ry to 
6
 chang
6
better
6
rated 
6
typing
6
action
6
screen
6
index 
6
 basic
6
. And 
6
ounded
6
 BASIC
6
atabas
6
ducate
6
realiz
6
suppor
6
e part
6
figure
6
certai
6
. That
6
chance
6
correc
6
troubl
6
friend
6
letter
6
NetDay
6
instal
6
 cente
10
 important
10
 did this,
10
. Overall,
10
demonstrat
10
nvironment
10
 literatur
8
 Compute
8
together
8
similar 
8
 chapter
6
ventur
6
 began
6
 descr
6
ithout
6
 acros
6
rolled
6
ournal
6
 error
6
FidoNe
12
Huckleberry 
20
The Lion, The Witch 
6
toring
6
early 
6
graphi
6
 expec
6
Julian
6
 since
6
lready
6
organi
8
solution
8
baseball
8
how much
8
 favorit
8
Apple II
8
 respons
8
bacteria
8
ad I not
10
census.gov
10
black card
10
. Occasion
10
 statement
10
participat
10
field trip
10
 org/wiki/
12
ing disorder
12
neighborhood
12
e one-on-one
12
 Ms. Esquive
12
Wildcat! BBS
12
conserve spa
DATA
Date: November 8, 2026 8:00AM Dear Vibe,~CCmorning, I went~Apis~Es~Cm/zenodo.org/records/7783660 and copie~AWtext~A4a book~FM is below: - -Y S~HJy E~IYd From~HW~BU a~Ad ~B- An autobiography~Ay~A5ing d~CJies in educ~AZBy ~H7I - j S~HJy E~IYd From~HW~BU a~Ad ~B- An autobiography~Ay~A5ing d~CJies in educ~AZCopyright 2020 ~H7Library of Congress Control Number: 2020917663 ISBN# 978-0-578-75560-1 Pricetag: $9.00 C~IIs ~!E2~!96~!A1 Prologue ~!E2~!96~!A1~Ab 1 - Introduction ~!E2~!96~!A1~Ab 2 - Elementary / Middle S~Bj~!E2~!96~!A1~Ab 2.1 - Personal Interests ~!E2~!96~!A1~Ab 2.2 - Faith and Leaming ~!E2~!96~!A1~Ab 2.3 - Ad~Ix~AP~Bd~Hm ~!E2~!96~!A1~Ab 2.4 -~Et and Math Leaming S~Hh~!E2~!96~!A1~Ab 2.5 - Reading ~!E2~!96~!A1~Ab 2.6 - Leaming~Ay~Itrs ~!E2~!96~!A1~Ab 2. 7 - Struggling T~BL S~Bj~!E2~!96~!A1~Ab 2.8 - Camp Palomar ~!E2~!96~!A1~Ab 2.9 - L~C2g~Ay T~Aj~!E2~!96~!A1~Ab 2.10 - Diagnosis ~!E2~!96~!A1~Ab 2.11 - Experiences ~!E2~!96~!A1~Ab 3 - High S~Bj~!E2~!96~!A1~Ab 3. I - N etDay ~!E2~!96~!A1~Ab 3.2 - Math Tutor ~!E2~!96~!A1~Ab 3.3 - El~Ggs ~!E2~!96~!A1~Ab 3.4 - BBS Hobby ~!E2~!96~!A1~Ab 3.5 - C~Ei ~!E2~!96~!A1~Ab 3.6 - Keyboarding ~!E2~!96~!A1~Ab 3. 7 - A~DV I D~Ab 3.8-Appearance D~Ab 3.9 -C~A- D~Ab 3 .10 -Resume D~Ab 3 .11 - P~DW D~Ab 3 .12 - M~H8 D~Ab 4 -C~A- D~Ab 4.1 - Leaming S~HhD~Ab 4.2 -English D~Ab 4.3 -C~EH D~Ab 4.4-Tu~I8 D~Ab 4.5 -Frustr~AZD~Ab 4.6 -Assessment D~Ab 4. 7 - A~GItes D~Ab 4.8 - Decision D~Ab 5 - U~CKy D~Ab 5.1 -Starting Over D~Ab 5.2 -NUAITP D~Ab 5.3 -Capstone D~Ab 5.4 -Gradu~AZD Photographs D Order form D Reference Prologue Thank you for tak~AP~BP to rea~AW~Dhe~CZ~B4 book~F4 you have~Axime, please visit my~Es at http://S~HJyE~IYd.com and fill ou~ATquestionnaire QK:b9192020k. I~EU~AJwrite~Aubook~Eu of~A7~Gy~Ds a~AT~H9g~ABel~Ek year~Bknt~AJprovide a re~Gz~AAs~Azs go~AP~Bdprimary~Ctecondary~A2l~A1 use to~Dcthem o~AHway~Ao~DF educ~AZand to hopefully~FF~DF~C7 or second ~B- a~ATend~Dje are~Argreat~GK~AAare avail~Bshelp~AQs~AAneed tips on~Btstudy and~Bt~GLe for~Ad.~CCi~GMone~AEos~Fis. I~IJ,~Auis a book~Ay my lif~B_w~AIwent~Ao~A4the beginn~F6~AS~E0~CEI~D_d my~DD~B-. I'm~A0not bring~F6~BW~BBin~DQ, only~Aatories~AAare unique to me and~AC~Fv to help~Bls. I hope you~EF~Aubook.~Ab 1 - lntroduction Five~HQ ago if you~COsked ~Bpwrite~Aubook, I~AC~A_said no. H~Bzafter w~AIsaw i~AHlast el~Ek~H- Donald Trump and Hillary Clinton, some~BBe~CWy click~Bymy mind. I can~Cn~BVimage of Mr. Trump mock~APe~Hpter, Mr. Kovaleski, not for his~Hpt but for his d~CJy. See~APis~IKrn~Cubecause a respectable, highly intelligent ~E1 can~A_their dignity squelch~Hqa man~BXMr. Trump just~Eu~Duare~CT~Djefore, my goal in writ~APis book is to~E2e a road map~AAothers can follow bas~FNhow~DQ~Iy~Dkow I've~FFd~A8al~GU~A4primary~A2l to~Ad. T~BLou~ATwriting~AEis book, I've drawn inspir~AZfrom several ~BDbooks~GV by ~D3~AMd~CJies. In a book~GV by Sam Sagmiller, entitled Dyslexia My Life, Mr. Sagmiller~HMs~Ay~Fjcident~AAhappen~AJhim in~A2l. He~Izibes a scene ~CEa~AQ~B0~Acssign~AJhis study~E3 to~Dchim an~AWother~AQs. At one~DM, the~AQ~B0~Frenraged. I~IJ of help~APe~AQs, the~AQ~B0 pick~Gha pair of scissor~Aln~I9stabbed Mr. Sagmiller~Ao the hand. Read~APis~FHupset me, but~CFI rec~Cga~Buin~C7~F8~Bemy~B0~GNd soap~Dt mouth for~BG~AAI said. So I ~IZe~AAat any ~EK~GW, es~HRly those~AMd~CJies, are vulner~Bsabuse. It is just~AAsome of us t~ILhide~AAabus~B_forget~Ay it~Aegood news is~Fw my~C7-grade~B0 told m~CS~AAshe had put soap~Dt mouth,~AF~H5 taken out of~AA~A3. H~BzI am~Ensorry~B9~AQs who cannot get out~AEese terrible situ~C0. I~B3~HXhe~AH~Ce~GDty,~AQs~AMd~CJies ne~AJstart advanc~BUward~DF~A8al obj~Ggs~Akhink pas~ATpast stigmas a~GIted~AMbeing ~Ce. Accord~BU a BLS graph in 2014, 16.4% of ~Ce~GOs aged twenty-fou~DNolder~A_a~DD~B-,~Dw 34.6% of non~Ce~GOs~A_a~DD~B-~FCnon~Ce~GO in five has a~As~HY,~Dw one ~Ce~GO in ten has a~As~HY. (People~AMd~CJies are~H6 to~A_~Dod a~DD~B-, (~D3~AMa d~CJy~H6 to~A_~Dod a ~EAs ~B-~Bf"Underemployment" is~Axerm~AAdefines~GE who is employ~Bya ~HZ~AAdoe~GMfully utilize~DF~Dxs. BLS~Hpt~By2019~AA31.1 % of ~Ce~H_es~GPd a~ATm~Dglevel,~Dw 41.1 % of non~Ce~H_es~GPd a~ATm~Dglevel. O~AH~BDhand, 20.7% of ~Ce~H_es work~Bys~F9 occup~C0,~Dw 17 .0% of non~Ce~H_es~A0work~Bys~F9 occup~C0. (Persons~AMa D~CJy: Labor Force C~HCistics Summary 2019t I~AH2018 voter turnout on ~JM~B8report tells us~AA27 .2% of~GOs w~I0~As~HYs voted,~Caas 65.7% of~GOs~AM~EA's ~B-s voted (~JM~BfThese factors show a link~H- ~Ce~GOs who vote~Dynon~Ce~GOs who voted based o~AHfact~AAnon-c~A--e~IYd~GOs are~H6 to vote and~AA~Ce~GOs are~H6 to~A_a ~B-,~FM~AC be a~Cqment to~EMa~ATm~Dglevel. A lot~ABprot~Eks~AAhelp ~Ce ~GW depend on policymaking~FC~F-ct i~BQIDEA act~AeI~Hals~AMD~CJies Educ~AZAc~BAestablish~By1990 to replace EHA~AeIDEA~EWsignificant~IMes to ~Iat~AQs~AMd~CJie~DHse~IMes focused~Cio~AHi~Hal tha~AHd~CJy. If, for~EP, two~AQs~COn auditory~EQ~JT, each~AQ~AC~D_ an IEP~AMtheir own plan~Algoals. As of 2006,~Cithan six million ~GW i~AH~Fgs~D_ s~HR educ~AZs~F9s~Ao IDEA (I~Hals~AMD~CJies Educ~AZAct~BfThis is why it's~In~AA~D3~AMd~CJies~A_a voice in~Hbs~CRing~AMvotes~AAmatter:~D4 acts play a~GH part in helping ~GW who ar~Ib~AB~Ce demo~I-c~F4 you~CR to look at some~ABp~DOs~AAwe're fac~BUday, most~A_to do~AMthe damaging~GXns~Ee~EWby President Trump. "The Trump administration's fiscal year 2020 budget~AC~FOcuts~I1s~Dpe agencie~Aloffices~AAserve Americans~AMd~CJies, stripp~APem of essential re~Gzs." (Trump's Budget Is Fu~CdCuts Aimed at People With D~CJies~BfThese~GXns are~B1atic fo~BV~Ce who ne~BFse~FT~BQmost. Unless~GE speaks fo~BV~Ce and votes for ~D3~AAcan put a stop ~Agse disastrous~GXns, ~Ce~Cpwill ~Hc to lose much-needed~FTs. So the question remains: how do we reverse~D4 policies crippling our ~Ce population? Eithe~BVnon~Ce popul~AZneeds to~CR vot~Eapoliticians~AAwill~IM~BKlaws to favo~BV~Ce, o~BV~Ce popul~AZneeds to rise up~C3dvance~His~DElevel ~CEthey can~FOvoting~GXns on~DF own~DKare~EEa few ideas~AAco~Bpmind~BeI~FB~Ay~Aup~DO. But~D4 are~A0~JEs~AAne~AJbe enacted~Hr now if we're~G1e equality among ~Ce and non~Ce~Cpi~AH~Fgs. In an effort to help, in~Aubook I~HM~Ay~DQ~A4birth~AStime I finished~Ad~Aeidea behind~Aubook is to share encounters I've had~Aoout~DQ~DKa~HS mea~FLmake anyone's life ~IN but rather inspirat~Ccstories~B4 journey to ~CEI am today~D0I will not be revealing~BW minute detail~B4 life, I will br~F6 some~ABmore~Bgting c~HCistic~Alalso some~ABencounters~AAI had. One~ABmost critical parts of~A7made me who I am is~AAI am incredibly goal oriented. If~B5's~BG~AA~Fb, I will~Hse it. I~FB~AAthis is one~ABhallmarks~B4 succes~Alwhat~FHdrove ~Bpcomplet~Eo educ~AZand enter~Eo chosen~E5r field.~Ab 2 - Elementary / Middle S~BjI~EXin~Ag world on March 24 at 10:58 p.m. a~ATU~CKy of~E-a Medical, San Diego. My half-sister, Kerrie O'Neal,~Ac talented musician and~AC~G2 spend much of her time~AMme o~AHpiano. She must~A_inherited her talent~A4my ~GR, who~EFed play~APe guitar and~AC~A_competitions~AMmy uncle, Curt,~G1e who~BMhe best musician i~AHfamily. My paternal~E_, Lester, was born in New Castle, Lawrence, PA, by John and Alice. John's ~F_s, Edward McCab~B_Sarah Ann McCabe (McAvoy), immigrated~A4Scotland~CHreland in 1884. My~Hn~E_, John McCabe,~CO very~EI life~Behis ~F_s died~CQas plac~Byan orphanage~Aeorphanage~Ac~FHbad plac~D7John~Dkis br~BDand sister. He~BMreated horribly b~Ecnuns. Early in~DQ, my m~BDand ~GR separated, mak~APings very~EI~EY~CS. H~Bzboth m~CS'~Al~GR's ~F_s (Alfred-my ~GR's step~GR, Noma-my ~GR's mother, Robert-m~CS's ~GR~ARBetty-m~CS's mother)~AYvery ~Iative. After~Aaeparation,~B2became~BI ~IN~EYy m~BDand me. My maternal~E_, Robert,~AC~C_me on exc~D2trips~I1s~E-a and Arizona~BH to Sequoia Nat~CcPark an~AWGrand Canyon. M~CS's ~F_s,~DF ~F_s~ARher sister's family~AYalways having get ~Ius. E~CWy, my uncle~AC introduce ~Bphis ~E1al~ANr an~AWgame Zork by lnfocom. Zork~Hotext ad~Ixe game~ESed i~AH1980s by staff and~AQs a~ATMassachusetts Institute of T~Ajunde~BVaegis~ABs~FU~Hty named Infocom. Zork uses an~Bnpreter~DA~AWZ-Machine~AAtakes~FIs~A4the use~DNdeciphers wha~ATuser~AC~BXto do. For~EP, ifthere~Ac lamp an~AWuser want~AJtum o~AHlamp,~CFa~FI~AA~EB~A1 try~AC be get lamp.~CCverb and noun combin~AZwas~Fxenough to~G3 the game to~Bhand wha~ATuser want~AJdo~BEn to~Ar~IP,~B5~Ac~A9reading. "An epicene gnome of zurich wear~BRthree-piece sui~ENcarry~BRsafety-deposit box materialized i~AHroom." (Zork~C8 M~Hdshow~CuZork on his PC.~Fc~BBwas cont~I2 b~Eckeyboard us~BRcombin~AZof noun and verb~Ebto~He ~IQs. My uncle~CRe~AWgame by selecting Zork~A4a text menu. As he~Io I~A1 hea~BVhard drive spin a~BQgame loaded. The~AH~IR lit up~AMparagraphs of text~CN~BNded a~Iziption of who~EWZork and~A7need~AJbe done~C7 to~Afess~Ao the game~Ae~Dhtask~BMo ope~AHmailbox and read~A7was inside~Aegam~BTdevoid of any ~I-cs~ARt~BYasn't a narrator do~APe~EC (rather~Aterson play~APe game~BMhe one~EC). If ~Fb~AJknow~A7~Gy~Ds~Dame, I~AC~A_to type look~ARthe game~AC show a li~Bxtext~Izibing~A7~Gy~Ds. I~AC ask m~Hdall~Axime~Ay~A7words meant~CQ~AI~A6do to~GF i~AHgame. It~DTme~G4 a whi~Cy~Ic out~A7some~ABtext~E7meant~BEn to~EC~Akyping,~B5~AYalso~Arpuzzle~HDs i~AHgame. It's not~BXa "pick your own ~IQ" i~B3~Afess~ASnext page~Dje~AYtimes~Be~DI had~C- done~C9~Id~B3 get~ASnext part. An~EP~AEi~CVi~AHmaze sec~DfThere~Ac~A9outside-the-box~FB~Crcreativity to get past~Aupart~Aeconven~Dnof a built-in map or a j~I3 wa~GMpart~ABgame~ARany mapm~DRor note t~DRhad~C-~Fdby hand. M~Hdhad pag~Fymaps on~E4s~Ek~ABgame~Cfa~Dw, I e~CWy leaned~Btget in~Ag hous~B_ge~ATlamp,~FM~AC be one~ABI key items need~AJprogress~Ao ~Edst~ABgame. All~AEis~DTsom~BP, l~Ei~Cjclue~Alask~Eo uncl~D7help, but~AF~Ep~A5~Crsome~CEbetween~HDar~Dimiddle~A2l,~Am~GFd~Ao almost 90%~ABgame. I've~F0 felt~BXI~FHwant~AJlearn~Btread~CQrite~FXuntil I~CL pl~GiZork~Dje~Acn incentive~AAby~Bhanding~A7~Gy~Ds, I~A1~Afes~Alfin~G5what~AC~F7n next. Zork was like~Aaimple form~GAbook, but~B5~Aclso~AUhallenge of need~BU know~A7each word meant~Aeopti~C4skip~Bwifficult word~CVno longer~B5~EYe.~Am to~Bhand~BW word i~B3~Bhand~A7~Gy~Ds~CQ~AIneed~AJdo to~FOmy next~GXn~BEn~Apat,~Am to~A5 to~HM words. Once I~Bhoo~AWsimple~Eblike~AUardinal dir~Eks, those~FIs~A1 easily be repeated; so in~AAsense,~A5~APe -- ---- - - -- - -------- -----~FI~CVnot~AAdifficult~GQit~Ep~DTti~Bplear~AHcommands i~AH~Dhplace~Akhen to repea~ATcommands~Hu~Ie's why~Bem~Hdgave me~AUommands once, he~I_ted ~Bpre~Dm them~Ip~Augame played a hug~Ib~Dt~ESment.~AF~HXhening not~EEmy vocabulary but~A0my~HTiz~AZ~D5.~Ab 2.1- Personal Interests In third~F8, I~ESed an~Bgt~C9li~Bxtoys~BZ"~IA." Each ~IA toy set~BNded~BW~BBneed~AJperform a spy~Hvity~Hw~AB~IA kits~BNded a camera~AMa mirror~AA~A6be us~AJtake pictures~Dacomer~Alalso a~G- dusting kit (Toys~ABeighties, Spy Tech~C8.~AF~G4 preoccupi~Hqmy new hobby, decoding~Ew~All~BrMorse Code. My third-grade~HDary~B0 knew~Ay my ~IA hobb~Dimy~Bgt in~FP~BRspy. She met~AMme o~Hx~ARI explained~AA~Fb~AJ~Iqe one~ABactivities i~AH~IA kit~AMthe~AL. I~FVmy~B0~Aapy-tech~G-ing ki~ENexplained how it~GPd~Aekit came~AMever~DG~AAwas need~AJboth tak~B_lift~G-s.~CCkit~BNded card~Alan ink pad for tak~Crpressing~Gms~BEn~B8kit came~AMa brush, powder~ARinverted ~JNs for tak~APe~Gms off an object. Befor~BKday~BeI~AC ~Iqe my hobby ~Ag~AL, five~AQs~AYselect~AJhelp me ~Iqe. In another~ALroom, I ~I2 each~ABs~Azs' fingers o~AHink pad~Akhen onto one~AB~IScards. Each~AQ had o~Bxseveral~G- patterns. Different patterns~AYus~AJmatch~Atrint. O~AHday~AAI~Dq~Emhobby ~Ag~AL, one~ABfive~AQs press~BFir fingers~Hust a flat object. I~BAthen my job~Fmgure out who touche~AWobject. Us~APe powder, brush, tape~ARinverted ~JNs, I~Iy to brush~Atowder on~Ag object~Cfa~Dw,~Atrint~Iy to appear~ARI~DTit off the object us~APe tap~B_place~AWtape on~Ag ~JN~Aewhite powder o~AHtape an~AW~JN gave~Gnclear image~ABprint. At~AA~HU, I~Iy~Fo~Cjeach~AB~IScards~AAI~DTfrom~Atrevious day~AMthe~AQ's~G-s on them and~Iy compar~APe lifted~Gm~ASfingerprint ~IScards.~CUr~AA~AGstaring at~E4card one by one, l~Ei a~ATdetails~ABprint~Egny~BBclose~DEmatch~Cfsom~BP, I stood up~Cttared at the~AQs.~Am an idea of whose~Gm matche~AWprint~AAI lifted, but~AF not absolutely sure. One~ABs~Azs giggled~ARby ~If I named this~AQ as my suspect. Sure~Gjh,~AF ~Igt~Dt identifi~BC. My~Bgt in spy tech~AC last a~Exlonger.~Am an~Bgt in investigat~APing~Aldreamed of o~Hx ~Csfo~BVFBI. I always~CO deep desire to~A5 abou~ATmechanical opera~Chof device~Alhow~Dufunctioned. Figuring out how~F5rical and mechanical devices~EMis~BG~AAhas stayed~AMm~D7a~Enlong time.~B6l~Ei a~ATceil~Crfollow~APe wires ~Agir destin~AZor to ~CEthey~DLed to, curious to~D9why they~AYthere and~A7they did.~AF~Bgted i~AH~C1~Alhow~B2operated~Ef today, I~EF l~Ei a~ATinner~Dbs of some~BBand seeing~A7makes it~He its task. As a young child, I~AC~A5~Bttake~B2apart, es~HRly~Beit ca~Bpmy~F5ronic toys. I~ERwant~AJknow~A7mad~BKwheels turn. I'd follow~Bvheels~DEbunch of gears~AAwere all~DL~AJa motor. I don't~FB I go~ATconcept of wha~ATmotor did at~AA~EKbut~AF curious as to why~Bvheels turned. Later on~Dt childhood, I~AC~Hy~Excircuits~AA~ADdo~CDs,~BH a simple alarm ~C1~AA~ADlet me~D9if~GE~EXin~FXme. I~EFed~Db~AMmy hands as a child. I es~HRly~EFed~DX outside~CXs~FCday, I ask~Emm~BDif we~A1~Hy a 2 ~!C2~!BD-foot deep ovalshaped pond i~AHbackyard. Using concrete and~Bq~BDmaterials, we turne~AWlak~B_stream~AAI dug~CGa pond~AMa small waterfall~C3 pump. Then we added~Bqfish~Hwthing~Ay see~APese ~Go~CXs~FPing permanent gave~Gnsense of~Hjhment. I~A1 observ~BKentire scope~ABp~DW~AF ~Cson~A4the ~HU i~BAjust an idea to~Beit~Fra fully ~Cs~C1~GG~BRwaterfall, pump~ARfish~Hz~AA~E0forward, I~AC sit by~Atond listening~ASsplash~ABwater as it traveled over~BSalls~Dw watch~APe fish swim~Ay~FCday, my~Flm~BDbought an~F5ronic Casio piano~ARevery week I~AC visit he~DNexplore my~E2ive musical~Dxs. I~CYknow what~AF~DX, but~Ao trial and~I4~AF ~Bscreate~BG~AMa melodic feel~Aeprocess of~E2~APe melod~Dihear~APe playback of~AAcre~AZfelt amaz~BU me. 1~Aclways se~Fqout~Exm~H8s~A4~CEI left off~Ie is why~GBt~AAthe~CXs~AYso significa~FLme:~AF~Bc~APe~EQ~A4start~Fmnish. I~EFed do~APe~CDs~BXpress~BRkey, hear~BRtone~ARthen pressing~Dpe key~Alhear~BRmelody. To me, it~Ac~GZ of expression in an~BDlanguage~AAonly I~A1~FOsense of.~Ab 2.2 - Faith and L~BrBefore~AF old~Gjh~Fo to Sunday~A2l a~ATLutheran church, I~AC sit~AMmy~E_ i~AHpew. I~CY~G0~Bhand wha~ATpastor~BM~Gp~Ay; so i~IJ, I~AC resort~D6odl~Ds~Bqscrap paper o~AHback~ABpew next~ASbibles. I'd~A0watch the organ player as he playe~AWnotes.~AF mostly~Bgted i~AHbuttons h~BTpushing. An assortment of~CT knob~Alpush buttons~AYscattered all ove~BVorgan as he~AC push~Akum them, produc~APe sounds i~AHchurch. E~CWy, I~AC~FPe old~Gjh~Fo to Sunday~A2l. During~AAtime,~B6doing arts-and-crafts~CXs~ARlater on I~AC rehearse a line out~ABBible~AAI~AC~G2~ETto all~AB~F_s i~AHchurch~BEn~Fo~BU church,~AF~A0t~DR~IB~ALes at a~H0c pool. My~Fl~F_s~CO pool~AAI us~AJwade in an~AW~IB~AL~AAI took~AC~Dcme~A5~Btswim~Damy~Fl~F_s' pool~AMmore~FZnce~Ae~IB~AL a~ATpublic pool~C6me~A5~Bqbasic~Hks,~BH float~Cr~IB.~Am built up~Ar~Face~ARwhen we~CL mov~BUwar~AWdeep end, I~AC~E7let go~ABside~Akread water~Dw all~ABother~AQs~AC cling~ASedge~BXwe~AYtold~D6~CfI ~IZed~AAno~BBbad~AC~F7n by le~Fqgo,~AF~Ciconcerned~Ay getting~CG~Ihe~HNany~BBelse if I let go. I~A5ed~BI~A4~IB~AL. And~Fw that, I~AC div~B_swim underwater~I1~BQfull length~B4~Fl~F_s' pool~Ef today, I~EF ~IB. So it's surpris~BU me~Bewe~AYtold in ~IB~AL~AAwe~AC ge~AT~If to jump off~Bwiving board.~AF act~Dll~Ei forward to diving~ARI f~Fe had my ~If~Bklk~AJthe edge~ABdiving board~ARthen I look~AJmy~Hr. I saw all~Atarents watching. I~CF~Gq a~ATin~G6or i~AHpool and froze. Whe~AHin~G6or asked if ~Fb~AJjump, I shook my head no~IUthen~AAwas it.~Am~DJ~BF~AL~ARI knew~Btswim. Fin~DSaroun~AWsam~BP, I w~GCup~ASfront~ABchurch~Ca~Atastor~AC lecture.~AF ready to recite my lines~Am~Fted~Fne. I stood up, face~AW~F_s~ARthen~CL spe~DRone or two~ABwords~ARI~Eya~ATt~Dd. She~CFhad to coach me o~AHrest~ABwords. As she whisper~BFm, I~AC repeat them~AS~F_s. I honestly don't~Cnr if I just~CYknow all~ABwords or if~Am forgotte~AHword~BQ~HU I walk~AJthe front~ABchurch~F4~AAwas~Cjwith,~Am a seat~AM~Edst~AB~GW~Hwtime~FXI~A5ed~Btswim,~AF~A0l~Brhow to ride a bike. I~CL~A5ing by rid~BRtricycle~ARthen e~CWy I mov~AJa bike~AMtr~HO wheels~FCday a ~Ii~EXby~ARwe~EU~AJsee if I~A1 ride withou~ATtr~HO wheels on. My ~Ii~CH w~GCup~ASvery top~ABhill~Dt backyar~Dycoasted all~Bvay~ASbottom.~CUr~AAit~Ac~A9fun~ARI kept~DX it~Cjand~Cjagain,~F1 e~CWy I became~FZnt~Gjh to bike~G7 own~Hz~AA~HU on, biking~Frmy~Gsr-one~Hvity. I~AC bike all ove~BV~JU~Ak~BL~AUanyons nearby. Whether i~BAthe~F5ronic~CXs~AA~AG~Cson or my passion~Egd~Ixe,~AF~ERout~DX~BG. Early on in~DQ,~Ckn't~A_any~BBlike a~ANr to pass~Axime. I~IJ, I~AC spend~Army~Bui~AHoutdoors~BEn to my outdoor chores, I~AC~A0bike~DEcanyon nearb~Diexplor~BKarea~Dje~Ac~GN i~AHcreek ~CE~Go crabs us~AJhang out~Dje~Aclso a tire swing~AAswung~I1s~AUreek~CN~Aclso~Atark ~CEI us~AJplay~C9~JF league~EOmy m~BDenroll~Cuin when~AF i~AHthird~F8. I~CL~A5~BU play ~JF us~BRtennis racket. E~CWy, I~AC~IC my hand-eye coordin~AZand upgrade~DEwooden ~JF bat.~Fc weekend,~D8am~AC~Fte.~AF ~HZ~Byleft field, so i~BAmy job to catch any balls~AAflew by~Akhrow them towar~AWinfield~BEn to~Ftes a~ATend of~E4week, we~AYalso pl~Giagainst ~BDteams~JO~DSI~AC~FOa home run. I~B3~Dcme~IC my coordination, my m~BDhad set up a net i~AHbackyard so I~A1~Fte at home.~CCwent on~Eg coup~Fzmonths~ARe~CWy~D8am won second~GN.~AF en~I2 i~AHnext ~JF league~F1 I e~CWy quit~D0my coordin~AZwasn'~ATbes~ENmy throwing~CBwork, I~Ep~ESed valuable teamwork~Dxs~Aoou~ATmonths~ARpl~Gi~JF did~Dcto~ES my hand-eye coordination.~Ab 2.3 - Ad~Ix~AP~Bd~Hm~Fc so often my~E_~AC~C_~Bphis cabin at ~Hm m~Gtn. It~Aclways~Arfun exploring~CQondering what~Acroun~AWnext tree. ~Hm~Holarge m~Gtn area in~E-a~AAis~Ay 13 miles long and 7 .5 miles wide~Aem~Gtn range runs northwest-southeas~ENis located past ~JA,~E-a~ARwest of San Felipe Valley (~Hm M~Gtns~BfEven now as I~HM this, I am in awe~ABlo~BC~AAmy~Fl~F_s~COccess to a~ATtime~Aeroad leading~AScabin~Acccessible~Ao a locked gate a~ATbottom~ABm~Gtn~Aewinding road lead~F6~Bm~Gtn passed Ironside Springs, a spring~AAwas constantly flow~AP~Bdthe m~Gtn and down~ASbottom~Aeroad was heavily forested~AMhuge pine trees on~E4side~ABroad. E~CW~Ecroad flattened out~DElarge clearing~FXe~CWy leading back in~Ag forest~Cfdriv~AP~Bdmore~ABforest~B8road split off~DEdirt road~AAwent into~AUamp site~Aedirt road~FApast an~F5ric-powered well,~Cja dry creek bed~ARto my~Fl~F_s' cabin. My~Fl~F_s' cabin~Ac large silver trailer~DL~AJa deck on stilt~DHre~Ac red fireplace o~AHdeck and inside~AUabin~Ac couch, chair, dining set~ARstove. My~E_~CH~AC often spen~AWnight i~AHcabin~JO~DSI~A1 hear~A7s~IV~BXa bobcat~Aebobca~ENm~Gtn lion are two animals~AAcan be~HP i~AH~Hm m~Gtns area~Aebobcat is commonly~HP in wooded areas~EOmake~BQ~Hm m~Gtns an ideal habitat for~Auanimal. (Bobcat~BfTher~BTplenty~D6 ~E6~AUamp. A small wooden ~GS crosse~BQriverbed~ARthere~Aclso a tire swing~JO~DSm~CS~AC come~AMus. I~A_a coup~Fzphotos,~BH one~Bei~BAsnowing~CQe're in~Bqsort of half igloo~BEn~AScampsite,~B5~AYalso two roads~AAsplit off, lead~BU a~GD~BCs tower~C3 vortac for planes~BEn,~B5~Acn area~BZCatfish Springs~AAwas covered~AMtree~Ala dirt road~AAcircle~AWarea. ~Hm~Acn amazing geo~I-cal wonder~AAwas predominantly a plac~D7hunters. H~Bzhunting~F0~Bgt~Cuas a child~JO~DSmy~E_~AC~G3 ~Bp~Gb firing his bow at~Bqhay bales.~Am~Arfun seeing how~Fkan aim~Am.~Ab 2.4 -~Et and Math L~BrS~HhA~A9work went~CGmy~Et and math~A5ing. One~ABstrategies~AAmy m~BDus~AJhelp me~Cnr my math~B1s~BMo~GN notes~Damy bedroom~Aeidea~BMo swamp me~AMinform~AZso I~AC retain most of it. Ad~CZ~DSshe~C5techniques to~Dcme~HTize ~Idn~Dpic~AZtables~Ez~Hf~AA~Evs of 5 are~ER~!C2~!BD~AB~Evs of 10. M~CS~A0tried~Czing ~Bpmemorize my~Dpi~BCs~CI flashcards. She~AC~HM~At~DO o~AHfro~FD~IScards,~AMthe ~Igt answer o~AHback so I~A1 quickly go~Ao~Ak~ILguess~AUorrect answer for each~B1~D0a~ATtime I~CYthink~AF~HTiz~Eo~Dpic~AZtables us~APis method, e~CWy~Ck~CR to grasp some~ABnumbers. Us~APis method in combination~AMsome~ABmath shortcuts~C6me~A5 quickly~BEn, o~Bxmy~B0s taught me~Btadd and~Dpy~Gsrs by counting all~ABangles o~AHnumbers~His. Five had five~DMs; the line o~AHtop,~Aahape~AA~Gq~BXan upside-down L~ARth~Ib~DL~APe L~ASC shape~B8C shape itself~ARthe final~DM an~AWend~ABnumber. By count~F6 all~ABpoints, I~AC be ~Bsadd or~Dpy any numb~CPfive~DKare~Hks~AAI use~Apis day. One~ABmo~HSabl~B_unique~Dxs~AAI~A5ed~Avird grad~BTcursive handwriting~CN~BMhe~EEtime~AAI~Cnr~A5ing~Btwrit~BKentire alphabet in~ID~Aewhole~EQ~DTa coup~Fzdays~AM~Gb sessions~BW hour.~AF given a handout~AMthe~ID ~Ijing~Gm~FNit~ARbelow~AAthere~AC be a line ~CEI~A1~Fte~Cw~BW ~Ij. I~EFed~Aupart~ABclass. Wr~D2in~ID ~Ijing was~Enrelax~Crsatisfying~Aetask was easy for ~Bpgrasp; I~Dzlike~AF~A5ing~BI~A4constantly tracing each~AB~Ijs i~AHalphabet. Of all~AB~A3~HEs~AA~Antaken~Avird~F8,~Et~BMhe most~EI~HE to grasp.~CUr~A5ing~Ay abbrevi~C0~ARI~Cv~AAI~AC abbreviate~BW word I wrote down~Ie~Hk, of~Ffe,~CYpass muster~Fw a~Dw, and~AF forc~AJt~ILspell out~BW word~Aedictionary was one~ABmain tools~AAI~AC use to~Dc~Bpspell a word. Back~CFwe~CYhave~ANrs to look up a word, so~Am~D6~BW~BBman~Dlby flipp~APrough~Atages. My third-grade~B0~AC~Dcme~Fw~A2l~AMmath us~BRvariety of visuals~BH a pie chart. With~D4~CT~Czing~Hks, she~AC vis~Dlshow me~Btcalculate frac~Chof a whole~Gsr~Ae~Cot methods of~Czing~AYquite helpful in showing me~Btdo math. Most~AB~DI~CUr~Apis day~Avir~Dyfourth~F8~AYth~JV~Bn~IQ~DHse~Bn~IQs~AYa powerful~Hk~AA~Eq ~Bpretain~AAknowledge.~CCi~GMto say~AAthe only~CDs~AF grasping~AYthese~Bn~IQs. For~EP,~Dw I~CYneed oneon-one help~A5ing~ID~Avird~F8,~AF~Ep ~Bsgrasp~ID~EOI use~Apis day~E7~DP I~EEwrote in~ID briefly. H~Bzth~JV~Bnac~Chdid~Dc~Bprecall~Aapecifics of how I~A5ed a new~Dx. Basic~DSwhil~JV~A5ing~AC be ideal, i~BAnot a~Cqment~EYe to~A5 some~BBnew at~AAtime.~Ab 2.5 - Reading I~AHsixth~F8,~AF~Bgt~Bybooks.~B6read~Ds~Fi~BZ~I7and The Wardrobe. I'd~Izib~BKbook as~Bqform of magical portal~DEdistant land~Aebook~Hofantasy novel for ~GW by~Et novelist C. S. Lewis, who wrote many of his famous works i~AH~I9to late 1950s~Aebook talks~Ay a magical wardrobe~AAus~HqLucy, one~ABmain c~HCs i~AHbook, to travel~DEmystical loc~AZc~CgNarnia~Aelandscape ofNarnia is~AAof a snowy woodland~GN~AMcreatures,~BH Tumus, a faun who be~Iis Lucy. (~I7an~AWWardrobe~BfI~A1 visualiz~BKworld~ABc~HCs; I~ETeach paragraph as ifl~AYstanding next ~Agm. For~EP, whe~AHbook~Izibed~AU~HCs walk~APrough~BSorest, I pictured~Erf walk~AP~Bda forest~Fxlik~BKforests i~AH~Hm M~Gtns. An~BDbook I read~BMhe Mark Twain~ALic ~I6Finn,~Ay a boy~C3 slave who rafted dow~AHMississippi River~Dw encountering some~Bgting c~HCs along~Bvay,~BH hitchhikers~Aebook was~C7~H0shed i~AHUnited Kingdom in December 1884 and~G2 i~BApublished i~AH~Fgs in February 1885 (Ad~Ix~Fy~I6Finn~BfNea~BVend~ABbook~B8boy, Huck~ARthe slave, Jim, crash~BFir raft~BEn ~Agse novels,~AF~A0in~Ag Goosebump series an~AWSherlock Holmes detectiv~Fi~DHre~AYalso~Bq~BDdetectiv~Fis~AA~AG~G0~CGa~ATtime. You~A1 say~AAthe days~B4~Bgt in spy tech~AYstart~BU come back. Books~AYan integral part~B4~A8. These~AY~Ed~Gzs~AAI~H1lly~AC not~D9existed except for~BSact~AAthey~AYnow avail~Bsme.~Ab 2.6 - L~C2g~Ay~Itrs O~Hx, m~CS's ~Ii show~Cua~ANr~AAh~BT~Cson~DA~AWC64~AeC64 was capab~Fzrunning user-created~AKs us~APe~IW~AKming~FYe~Ae~Dhshowing~ABC64,~Blwise known a~BQCBM 64,~BMhe week of January 7, 1982, a~ATConsumer Electronic Show, in Las Vega~DH~ANr was market~AJhouseholds as an 8-bit home~ANr (Commodore_ 64~C8~AeC64~CO built-in~FWd~Dje~AYalso ~G_ cartridges~GGing~AK~DH~ANr~DL~AJa TV set us~BRcomposite~F2~BEn~B8C64 came~AMan external floppy drive with~AKs - -- ~Aw--------~BH GEOS. GEOS~BNded a word~EQing~AK~BZgeoWrit~B_a paint~AK~BZgeoPaint~Aeuser~Bnface had features~AAwere ~Iv~Ag~ALic Mac OS~BH a cursor, menus, window~Alicons. (GEOS 8-bit oper~Fp~C1~BfI spent hours~Cw~IW~AKs. It~Acs if the~AKming ideas~Dt head~AYinfinite. One~ABp~Ais I wrote displayed a menu o~AH~IR~AA~ADallow the us~CPselect~EG~BCs to run. I~BZit "2menus," and i~BAbas~FNa~AK~AAm~Hdus~Byord~CPrun Zork. I~A0created a coup~Fz~I-c~AKs~AA~ADcreate vectorlike lines o~AH~IR. To me,~AKming~Dzlik~BKone~CD~AAI~A1 ~G0~Bhand. P~Aiming~G3ed ~Bpknow~BeI did some~BB~Igt and~BeI~EWan~I4 (since I~AC~A0se~BKoutput~B4~Hg o~AH~IR)~Aeway I --- ~Aw ~Aw---,.,~BXto imagine~Auis~Apink~Ay how an~H2~AC be~F8d. W~I0 a~ANr, you won't know~A7mistakes you've~EWuntil~Fw you submi~ATpaper for review. With~AKming, the~ANr will~F8 your~AK~H5. Being a~AKmer is~BXbeing~B0 and~AQ a~ATsam~BP~ARthat's~A7~G0~C6advance my~AKming~Drfrom~AA~E0on.~IW was~F3n~AJbe an easy-to-use~FYe. I~B3 enable non-scientist~Alnon- mathematicians to use~ANrs, John G. Kemen~DiThomas E. Kurtz~F3ne~AWBASIC~AKming~FYe in 1964. (BASIC~BfBasic was~BNded i~AHC64~ANr. P~Aim~ED~IW~Ac lot~BXbre~DRdown an idea line by line. Conside~BVidea of~EV~ASstore to buy~Bqmilk. A ~G6ured~HFe may go~Ao~Atrocess of~EV ou~ATdoor, w~Gp~ASstore,~CFopen~APe door~ASfridge~GGing milk~Cfopen~APe fridge door,~Aatructured~HFe~AC ~Hc on~Ao~Atrocess of pick~F6~Bmilk and f~Fe p~Gifo~BVmilk a~ATfront counter.~Fc step is broken up~CGan in~G6ion,~E4written~BXa single~JP or~FI. Similarly,~Ebtell~BRdog to sit, bark, lay down are~A0written~BXi~Hal~JPs~F4 transferr~APese~JPs~CGBASIC, the~Bnpretation~AB~G6ured~FYe is~GV~C9language~AAcan be~ETby a~AKmer~Aelanguage~AAcan be~ETby a~AKmer~AC~CFbe compiled~CGa~FYe~AAthe~ANr can then~Bhand. I~CR~AJuniformly mold~DQ~Dasimple~JPs as if I~AYwr~D2a~AK~Hw~AC see~Auas a~Exbit robotic~GQI~A0found it~Cisecure at times when~AF dealing~AMa new~IKpt.~Ab 2.7 - Struggl~AP~BdS~BjEarly on in~DQ,~B5~AY~Idn~CDs~AAI~A1~Bhan~Dy~BD~DI~AAwere too complex~F4 it ca~Bp~Idn~HEs in~A2l~Ez math, I~AC~EF the ~H9g~ABclass~F1~Aaubject~Frmor~B_more complicated as~Banal material~Acdded~CN~BMhe breaking~DM ~CEI~AC tend to fall apart-at least when~AF a kid. As an~EP, let's say~AAa~B1 asks ~Bpcome up~AMthe average~GAset of~Gsrs. In my mind,~AF~FBing~Ay~Aulike a~AK. I~AC ad~AW~Dhnumber~ASsecond~Gsr an~AWsecond~Gsr~ASthird~Gsr,~F1~Am gone~Ao all~ABnumber~DHn depend~Ds how many~Gsrs t~BYere, I~AC~CFdivide~Axotal sum by~AUou~FLge~ATaverage.~Fc~AQ had~DF own~EQ of~A5ing.~GBt~Cicompell~AJdo it~Auway, not~Hf ~JG time~AF act~Dltaking. E~CWy, I~AC fall further behind do~APings~Auway. Soon~AF spend~De hour~DX~BG~AA~ADnormally~C_someone thirty minutes. But~EYe,~Aufelt~Cinatural by break~APings down~CGlogical steps. A lot~AEis~EXdown~ASlack of~FZnce~Am~Dtself. I'd run in~Agse~B1s ~CEI might doublecheck an equation~AA~Andone,~E7~DP I~Heed~AUalcul~AZ~Igtly~BSirst time~Dje are those who can~A5 some~BBright awa~Direpeat it w~I0 go~APrough~Aateps. H~Bzfor~Bls~BXmyself, i~BAmore comfort~Bsgo~Ao~Atrocess slowly~GQi~B3 catch up, ~Gl~AJbuild up a ~Go~Ci~Face~E7if~AAl~AJmore~Hgs. All~AEis~DT~Gb~ARe~CWy I~AC get ~IN.~Ab 2.8 - Camp Palomar I attended camp i~AHCalifornia Palomar M~Gtns~Eg week dur~APe sixth~F8. Al~DP I~EFed nature an~AWweekends~AMmy~E_ at his cabin i~AH~Hm M~Gtns,~AFn't sure ifl~AC~EF spend~BRweek~AM~D3~AF a stranger to. Nonetheless,~Ck atten~Dyenjoye~AWwhole week. O~Bxmy~JHe ~HUs~BMhe long hike~Ao~Aubeautiful natural setting.~CUred~AAther~BTone~DM w~BYe~AYclimbing~Cjboulders~ARas I watched~A4above, I stretche~G5my hand to~Dcone~ABgirls up ove~BVboulder. I~Dzthrilled~Ee~Danatur~B_~Csas a~E8to overcome obstacles. All~AEi~CVpart~ABfun and~EFment~ABhike. As we reache~AWtop~ABtrail, one~ABcamp leaders hand~Cua pl~Gicard as a reward for help~APe girl. I~F0~I_ted I~AC~D_ an~DG, and~AF grateful once~Hu. I~EFed~Ee part~ABteam~Dkelp~APose~AAneeded a hand. It reminds me~GAparticular incident in~As. It~Ac rainy day~ARall~ABkids~AYtry~BU stay dry. As I walked~Ao the hallways to my next~AL, I noticed a~E3 of teenage girls running i~AHrain. They~AYrunn~BUward a teenage boy who~DBtuck i~AHrain~G9wheelchair. I~CYthink twice~Ay~A7~Gy~Ds~Aeonly~CD~AF~IKrned~Ay~DBt~Gidr~Digett~BU my next destina~DfH~Bzthe girls saw~AAthe boy was in ~Ih~B_needed help. I ~DPt~Ay~Au~HU an~AW~HU~BeI helpe~AWgirl o~AHhiking trip. In both instances,~B5~AY~D3 ~E6~ARonly one ~E1 or a~E3 of~Cpsaw~GE~AAneeded help. Lik~BKgirls an~AWboy i~AHwheelchair, no one else~DTit upon~His to help. For~Bqreason, it~F0 enter~Emmind~AAI~A1~A_gone out~Dkelpe~AWgirls. I don't~FB it's wrong~AAI~CYhelp~Dvi~BAsomething~AAtook som~BP for ~Bp~IZe why.~Am~A5ed~BI~A4my trip to Palomar~ARi~BAo~Bxmy fondest~HTies in~Gke~A2l. While~AFn't sure ifl~AC~A_enjoyed it at~C7,~AAquickly~IMed. It~Dzlike~Am walk~AJthe end~ABdiving board anticipat~BRdive; but i~IJ of w~Gp back, I~DTthe leap. Camp is one~AEose~Hvities~AAany child may be unsure of ~JQ~ED~Dje'~BQanxiety of~Ee stuck somew~BY~I0~Ee ~Bsreturn hom~D7up~DEweek. Yet,~BWone a~ATcamp wants~G1e~BWone~EF~DF time~Cfall, happy campers mean~AA~A3s will want~DF~AQs to come back next year. I~AHend,~Am~EF~Emtime~ARI am grateful~AAI~EU~AJ~JQe.~Ab 2.9 - L~C2g~Ay T~AjIn sixth~F8, my~B0,~JWl, noticed my~Bgt in~ANrs. Back then,~AF spending~Bqof my~Bui~AHt~Aqy~ALroom ~CEthey~COn ~JI~ANr. Steve Wozniak was~Atrimary~F3ner~AB~JI~AMSteve Jobs a~BQoverseer~AB~JI's foam-molded plastic case. A~ATtime of production, i~BAquite~GUful as an 8-bit home~ANr. (~JI~C8 I aske~AWt~Ajt~Dd if~B5~Ac way~AAI~A1~HM~AKs~CI a~AKming~FYe~Aet~Ajt~Dd show~Cua~AK~AAhe~Acb~Cycopy~DEfloppy disk~AAhad~AUapability of running~IW~AKs. Back in~JWl' s~ALroom, I~DTw~AIl~Cl from~Ax~Aqy~AL and~Dqed it~AScurrent~AL as~DZ a~Dqa~DfI inserte~AWfloppy disk~GGing~IW~Ctome~ABp~Ais~AA~Anrewritten~A4code samples~AAwere~BNded~Hz~B5, I went~Ao~Bmenu sel~Ek ~IR~Ctelected some~ABp~Ais. One~ABp~Ais~Ac~Ew~AArepeated, "Hello World!" I explain~AJthe~AL~Cbhis~AK was~GV~AMa few lin~Fycode and~AAit~FVhow~FI~Allogic~A1 be us~AJdisplay a~Ew o~AH~IR~Dje~Aclso a vector line ~I-c~AK~AAdisplayed triangles o~AH~IR~Aepoint~ABpresent~AZwas to show the~AL w~AIhad~A5ed~Ay~ANrs and~AAcommands~AYus~AJtell a~ANr~A7to do~BEn to~AKming,~AF~A0checking out~ANr~GK from~Aachool's~IE.~AF~A5~APe~ITs of~ANrs.~JWl was~G4 impressed~AMthe~Dq~AZand told m~CS~Ay what~AF~DX. She felt~AA~Gl~AJpursue my~Bgt in~ANrs and~AAo~Hx~Auinterest~A1~ES~CGa~E5r. My m~BDwas eq~Dlimpressed~AA~AGdoing so well. She~Eyinto~A7she~A1 do~EY~B_end~Ghge~Fqme an AST Advantage~ANr~CN~ANr~AC last me~Ao~As and~DZ~Ad. It ran ~Gc3.11~Dkad tons of~EG~BCs on it~BEn, she~Acb~Cyget me an Epson~Gm~CPgo along~AMthe~ANr. One~ABclasses~AAI~DTin~Gke~A2l~Ac t~Aqy~AL~AAwas focused on~BW~BBfrbm woodshop to~F5ronic~Al~GS~Hying. The~AL~DBet up so~AQs~AC~EMon one st~AZfor a coup~Fzweeks~Akhen move to an~BDst~AZand~EMon~AAfor an~BDcoup~Fzweeks. Each lesson was on VHS.~B6~Cson~BI~AB~Cot st~C0. One~ABsta~Chinvolved~E2~BRsmall wooden air-powered dragster~AeVHS video fo~BVdragster st~AZt~GCabou~ATdesign~Dkow air resistance~AC play a role in how fas~ATdragster~AC go. I~CL by~F3n~APe shape~ABcar~CI~Bqpaper,~Ejg~CGconsider~AZ~DI~Ez air resistance~EOI~A5ed~Ay~A4the video~Cfthat, I~AC sharpe~AHcar us~BRvariety of wood-sculpt~BUols.~B6hav~BU go over~Aahaping part~BI~EOseem~AJtake forever. I~AHend,~Am a~Excar dragster. ., O~AH~GS-building station,~AF give~AHtask to~Hy a ~GS us~BUothpicks~Aevideo went~Ao~BWthing~A4t~Gp abou~AT~H3y of engineering~ASsteps~AA~ADne~AJbe~He~Byord~CPcreat~BK~GS,~BH how all~ABparts~AC be glued ~Iu~Aevideo~A0talked~Ay why~Bq~GSs~AYstronger than~Bls an~AW~Cot typ~Fy~GSs~AAwere used,~BH long road ~GSs crossing river~Aloceans~DEsmall foot~GS crossing stream~Alsmall rivers~Aevideo~A0went~Ao the ~H3y of ~GS~Hy~Crhow~AUon~G6ion of ~GSs became~Ciintricate~Cjtime. Once I con~G6e~AW~GS us~AP~Ibs~B8bridg~BT~Do. Weights~AYus~AJmeasur~BKweight limit~AB~GS. Adding varia~Chof triangle shapes us~APe toothpicks~A1 aid in~HXhen~APe ~GS.~AF~A0~Bsbr~APe ~GS home once~AF~Fdm~DRit~ARit lasted~Eg~Dw~F1 e~CWy it fell apart. Fin~DSI~DTan~F5ronics~AL. In this~AL, I~C5an assortme~FDresistor~Al~BDsmall~F5ronic components~D6~CDs~A4controll~APe brightness~GAlight to controll~BRmotor. A lot~AB~DI~A4the~F5ronics~AL~AY~DI~AAI became~Bgt~Bydoing~G2 on~Ez us~APe 555 integ~IOcircuit (IC) chip to~E2e a time-keeping device. I~A0designed a~HlIR transmitter~AA~ADtransmit audio wirelessly~Ao the air~DE~Eler~DL~AJa speaker. By se~Fqup~Dpe IR~D_rs~Aoou~ATroom,~AF ~Bsexpan~AWaudio~A4one audio ~Gz. I~A0took it a step furthe~DNimplemented an~Bncom ~C1 us~APe same~Hlcircuit. I~EFe~AWhands-on~A5ing~A4this~AL~ARthis~Bc~BTo~Bxmy~JHes in~Gke~A2l.~Ab 2.10 - Diagnosis~B6in~Gke~A2l~AAsome~BBdi~DUfeel~G4~Hr.~AF quie~ENreserved, maybe a bit shy. I~CY~IZe how far back~AF~Dje~AY~Idn~HEs~AA~AGgood at and~Bls~AAstumped me~Dje~AYtimes~Bemy~B0s~AC~DM out~BG~AA~Andone~AAwas obviously wrong: it~Aclmost~BXmy mind was~C9fog when~AF do~APe assignment. It~Dzbad, almost traumatizing, hav~BU witness all~B4~I4s when~AF~GZ~Hjhed i~AHwork~GBt~AF~Fkat~EOwas~AKmmg. I~A1n't~Bhand exactly~A7was wrong a~ATtime~Aeonly ~BBmy m~BDknew was~AA~Anbeen evaluated at UCSD pediatric neurology~Eg severe~ESmental speech delay when~AF~Ay three~HQ of age.~Am undergone speech therapy to ~Igt~Audelay. In~HDary~A2l~Daor~Fne~AF eight,~AF diagnosed~AMauditory~EQ~JT. I~D_d~Arhelp~Aoout my~A8~EOsignificantly help~Cu~GJe my goals~Dvit~ACn't be~F1~Ad~BeI'd~Bhan~AWfull effect~B4 impairment~D0not~BWone gets to~Bce~A7happens behin~AWscenes~C9s~HR~A8~AL unless they've work~Byan educ~AZsetting, my~Bc~BTquite unique bas~FNmy~Bce. Back i~AH1980s, a~AQ~AMs~HR needs~A1~A_been mainstreamed~AM~Edst~AEeir~ALmates or segregat~Bya much smaller~AL among~AQs~AM~BDvarying but ~Ivd~CJies~BH ADHD, dyslexia, etc. One~ABprimary tools~AAthe~A8al p~CMl~AC use to gauge the~Afess~ABs~Az~Ac test, ~F-~BQWoodcock-Johnson Test~CA~BI has~IMed~JB~AAtime. Most~AQs~AMd~CJies are no longer segregated but i~IJ mainstreamed in~A7are now~BZ"inclusion~ALrooms" (~ALrooms~Ca~B5~Homix of ~Ce and non~Ce~AQs)~D0I'm not exactly sure when~AAhappened, my guess~AC be sometime~Fw 1992. At~AA~EKa~AQ~AC~Ep~A_a re~Gz room, h~Bzthey~AC~A0be tak~APe same~ALes a~BQrest~AEeir peers,~BH math~Dkistory. Along~Bvay,~B5~AC~A0be an aide or a~B0 who~AC be~B5 to assist the~AQs~Avose~ALes, es~HRly~Beit came~ASmore~EI~ALes~Ez math~Aemost~GYging factor~AEis transition to~BNsion~ALrooms~AC~A_bee~AHincreased~AL size, as~Auposed~Ciof a distr~IQ~ASl~C2g-~Ce~AQ. The~AQ~AC~A0~Ele help~Ao~DF contract known as a 504 plan or i~Hal educ~AZplan (IEP). These~AQ contracts~AC lis~ATtyp~Fyaccommod~C0~AAthese~AQs~A1 use~BH a quiet e~Ir, among~Bls. H~Bzif the 504 or IEP plan wasn't~Gjh, the~AQ~A1~A0have a modific~AZin~DF~Ffes.~CCcame~AMa catch, ~DP,~Eu by tak~APe easier path, the~AQ~AC no longer be eligibl~D7a~HY. They~AC~A_end~Ghneed~BU take~ALes as an~GO~AQ if~Duwant~AJ~Ele a GEb and go on to~Ad. I've~BNded~Banal~Gum~AZon my~Bce~AMauditory~EQ~JT~Ct~HR educ~AZo~AHfollowing~Es: http://s~HJye~IYd.com/978533 515~Ab 2.11 - Experiences Go~AP~Bdmiddle~A2l~Ac lot~BX~Gaout of~GN. I sensed a constant~GZ of disapproval among my peers. I~Dzsafer by not ~JQ~ED case I said~BG~AAothers~AC tease me~Ay. Other times,~Grw~A7to say, I just~CYknow~Btsay it.~AF~G4 cautious~Beit ca~Bpsocializing~AMothers. My~Hnest fear was~Ee outed as one~AEose~AQs. What~AC~Duthink of me or say?~AF ~IN off keep~BRtight lip and my head down~Dw focus~Ds~DX my own work. On~BP in biology~Ax~Dd had us break~CGgroups.~AF paired~AManother~AQ~AMa~A5ing d~CJy~Akwo~Bl~AQs. At one~DM, one~ABs~Azs stepped out~ARthe~AQ~AMthe~A5ing d~CJy~EWan alter~AZto~A7we~AY~Cson. Whe~AHs~Az returned, she said, "Did you do this?" I ~DP~ATalter~AZ~Gq good~DvI~A1 tell from~Axone~AAthe~AQ ~DPt~CTly~Ie~Ac day ~Fb~AJend as soon as possible. I~BAbad~Gjh~AAwe~AYstruggling~Dvit did us no good~C- treated~AAway by another~AQ. Like a~H1l fish~C9lake fu~Cdpiranhas, most~ABtime it's bett~CPjust hide. In~Gke~A2l,~Am a bully~AA~ADt~ILannoy m~B_sometimes~E7injure me~FCtim~BKbully~C3~E3 of kids corner~Cua~ATfront~ABPE~Hying.~AFn't as much scared as~AF confused~C3ngry.~Am~FQhurt by his aggressive behavior toward me~Fne.~Grw a~ATtime~AAall ~Gl~AJdo~BMo stay put~Dkold my emotion~DHr~BTno~D1for ~Bptry~Fmght back, so I remained calm~Cfreflect~Ds~AA~HU in PE, I wondered why they~AYacting~BXthat: it~Aclmost~BXthey~AYtry~BU get a re~IQ~A4me~Hz~AA~E0forward~B8bully~AC ~Hc to t~ILprovoke me as if he wanted ~Bpfight back~Dvhe~F0 got his way.~Am a ~Ii in PE~AL who I us~AJtalk to~Dkang out with. He~Ac brilliant~AQ who was en~I2 i~AHadvanced AP~Ffes.~B6on~BP h~BT~El~De award~CQear~BRsuit~Akie. He was~Axype of ~E1 I~AC~A_want~AJbe a~ATtime. I believed~AAhe~Ac motivator~AAlead me i~AHdir~Ek of want~BU~FF~GU. Being his ~Ii~EWme~FB~CTly:~B5~Ac way to break out~ABmold~AA~AGin,~E7if i~BAjust to look~C3c~ATpart i~B3 hide my defects. Later on in~DQ, I~AC begin to~Bhand why~Bq~D3 had~Ausuperior attitude~Ay~His. Not~BWon~BTlike this. I met~Arcool kids along~Bvay~BEn,~AF~Enquiet when~Am~B1~Aldi~DUspeak up~EYyself. I~IJ, I tri~AJhide my~B1s, hop~APey~AC go away.~CCis a~B1 I~Ep~A_today; h~Bzover~BuI've~A5~AJspeak up~EYyself, even~BeI~Dznervous~DX so. I~FB~Auis~BG~AA~GW ne~AJlearn~Btdo~ARthey shouldn't be afraid~D6 it. Ifl~AC~A_known~Btdo thi~Alha~DUbeen afraid, none~AEese unpleasant incidents~AC~A_happen~ByPE. Thankfully~AUoach saw~A7~Gy~Ds and put an end to it. I am truly thankful for~Ax~Dds~AA~Iated me~Ao~Gke~A2l~E7~DP~AF a reserved ~E1. A~ATvery end~AB~A3 year,~Bmiddle~A2l held a gradu~AZfor all the~AQs who~AC be~FJ~Crgo~Ds to~As~CfI pick~Ghmy gradu~AZc~EH, I rejoin~Emm~BDand~Flmother.~Am gained~Arknowledge from~Atast~HQ at~Gke~A2l~CN~BMhe~C7~BuI~A5ed~Ay~ANrs and~ANr~AKming. My~Bgt~AC~FPe permanently ingrained~ARI~AC ~Hc to study more~Ay~ANrs.~Itrs~AYstill a novelty an~DUseen in many households~BEn, the~Bnnet~AAwe~D9today wasn't~E9.~AF~ITally grow~F6 dur~APe emergence~ABinternet~Akaking~Bgt in~Aunew emerging t~Aqy~CN~Bgt gave~Gnreason and purpose~C- at~A2l,~JB this~AC be~Atlace I~AC~A5~Ay~Aunew t~Aqy. A~ATvery end of~Gke~A2l dur~APe summer, I~DTone final~Gke~A2l~AL to~FG~Erf for~As~CN~AL~AC brush me up on some~Banal~Ehwork~AAI~AC~D1for my freshman year.~Ab 3 - High S~BjBy~Axime I~EWit to~As,~AF~CR~BU make~Afess. A~ATvery ~H9g of~A2l,~B5~Acn event~BZ~Ik '97~CN~Ac ~HGr event ~CEfacult~Di~F_s ~HGr~AJ~Ill~BSirst~Bi infra~G6ure in~Ag~ALrooms.~Ab 3.1-~Ik I~B3~DL~A2ls, libraries~ARclinics~ASinternet, high-tech ~Hties~AYc~Cg~C4commit re~Gzs to an event establish~By1995~BZ~Ik '97.~CCran~H- 1995 and 2004. (~Ik~BfNonprofit entities~BH N~DC Solution~AlSan Diego~Itr Society~AYinvolved i~AHplanning~AB~A3's~C7~Bnnet~DLion,~FM~AC be inst~Cgon January 11, 1997. A~Bi~FT~FEr~Ez a~F2~FEr or a~Hlcarrier is~AUompany~AAprovides you~AMa~DLion~ASworld wide web an~AWinternet~Ae~A3's~Bnne~BAfunneled~Ao cert.net, a mid-level~Bi~FT~FEr locat~ByCalifornia (CERTNET~BfThis~FT~FEr supplied ISDN, T3 and Tl to businesse~Al~A3s~BEn, CTSCOM, a dial-up PPP~FEr was~C5as a backup~Ao a Win NT~Fvr. CTS.COM~Ac DSL~FEr in Southern~E-a. Various t~Aqies are us~Bya~Bi to physically~DL ~C1s~BH~ANrs~ASn~DC. An~EP~AEis~AC be~AUord~AAplugs a desktop~ANr~DEcable modem. Keystone RJ45 adapter~Hotype of adapter~AAlooks~BXa phone jack. Patch panels look~BI~BXa row of~Dpe phone jacks and~ASlaymen,~Duappear to~EMjust~BXa phone jack~AC~Ae~HGrs work~FNall aspects~ABn~DC~A4drill~APe holes~Ca~AUables~AC be f~AJpunching dow~AHcables in~Ag RJ45 terminals an~AWpatch panels. It~Ac huge~EQ involving~Arplann~Ded~Ac~Hn~A5ing~Bc~D7me~F4 I arrived, my job~BMo~Dc~Ia~AT~BD~HGrs.~AF given a MURS 2-way walkie-talkie and~Acsk~AJcheck in~AMeveryone~CN~BNded bring~APe ladder~AS~HGrs~AAwere string~APe~F2s unde~BVeaves~AB~A3~D0my part di~DUplay i~AHtechnical aspect~ABn~DC,~AF~Ep~A5ing~BI from~Au~Bo. I~AC watch a~BQ~HGrs punche~AWwires in~Ag keystone adapters~Dw ~BD~HGrs drilled holes~Ao~Bvalls to feed~F2 in~Ag~ALroom. Afte~BVeven~BAover,~AF ~Bs~Hc to assist~AMthe~Bi~Cr~BDaudio/video~Ak~Aja~BQassistant fo~BVMedia S~HRist,~EZ. At~AApoint,~AF the one~JJibl~D7~Iheshooting~B1s~AMCTS and CERT along~AMSDCS and NS in~Ban to feed~APe RJ45~F2s from~Atatch panels to each~ABclassroom. I~C5various~Bi~BUols~BH a~F2 test~CPmake sure~AAt~BYeren't any fractured wires in each~ABcables~AAI ran. I~A0used~F2 crimp~CPput ends o~AHextra RJ45 Ethernet~F2s to~DL~ANrs~ASkeystone wall plate. A~AT~EKwe~AYnot~CI DHCP(A protocol us~AJautomate IP~Bi configur~AZ~CEin a non-DHCP based~Bi, each~ANr~AC ne~AJbe man~Dlcon~Icd) to allocate IP addresses to each~ANr, so~E4one had~C- man~Dlcon~Icd~AMa static IP address. O~AH~DhSaturday of~BW~HV,~EZ~CH~AC do some~ABlarger~F2 runs~DLing~Banal~ALrooms~ASn~DC~Akroubleshooting some~ABissues~AAthe~B0~Alstaff~AYhaving. We~A0~Iate~AW~A3's AV needs~BNd~APe auditorium,~BSield~AMsome large audio equipment~ARsome~ABp~Ais~AAwere~F7ning during open house. I~A0work~FNupdat~Cr~Illing s~FUi~AH~AO lab~Aehigh~A2l math~B0 a~AT~A3~CO coll~Ek of older PC~Ala Novel~Fvr~AAhe brought~Cj~Ag~ANr lab to~A_me set up~B9~AQs~Aeolder PCs~AC act~BXdumb~Gvts~AA~AD~Ele all~AEeir~Ebfrom~AUentral Novel~Fvr set up i~AHback room~DKdumb~Gvts~BZthin~Gvts in~Aun~DC~AYlightweight~ANrs~AAwere optimiz~AJestablish a remote~DLion~AMa~Fvr-based computing e~Ir~Aesoftware~AKs,~Heing calcul~C0~ARs~I8 data work~ABn~DC was~Fdo~AHserver side~ABn~DC. (Thin Client~BfEach~ANr was~DL~AJa I 0BASE2~Bi hub~ARan~BDcable ran in~Ag back room~AMthe Novel~Fvr. Novel, Inc.~ESed a~ANr~Bi oper~Fp~C1~AAhas~JB~FQdis~Hcd~BZNetWare~Ae~C1 use~AWIPX~Bi protocol i~B3 run various~FTs on ~E1al~ANr~CI cooperative multitasking. (Netware~C8~AeNovel~Fvr ran~Dpe~EGc~AZinstances in~BG~AAresembled a virtualized~Fvr. If one~ABs~Azs i~AHlab want~AJopen up a word~EQor on one~ABdumb~Gvts,~DF~FWd input~AC go~ASNovel~Fvr's "Virtual Client" an~AWvideo feed~AC travel~A4the Novel~Fvr back~ASdumb~Gvt~AAthe~AQ was~CI. In essence,~D4~ANrs i~AHlab~Cqd minimal hardwar~B_a 1 0BASE2~Bi card. All~ABpower~DBtored centrally i~AHNovel~Fvr~Aep~DW~CYwork well fo~BVneeds~ABs~Azs. We a~JC had~Arperfectly~Fkolder Mac~ANrs~AM~I-cal~Bnfaces an~AWdumb~Gvts~AYquite primitive ~I-cs wise. H~Bzit~Acn~Bgting~IKpt~ARit~FVa~ATtime~AAa ~C1~Ez a Novel~Bi~A1 work~Dje~Aclways~BG~AAneed~AJbe~Fda~AT~A3~AAinvolved~Bqkind of t~Aqy~ARthis gave me~Ar~Bo~Db~AMa variety~AEings:~A4n~DC~BUols~BH crimper~Alwire testers to~ANr~AKs~BH defragging~AK~Aldisk-check~AKs.~EZ~Aclso a~Hn mentor. He~FVme~Cbo~A_a p~CMl attitude toward our customer base,~ERm~DRsure~AAthe customers~AYsatisfied~ARhe~A0showed me~Art~Ajove~BVyears.~Fc aspect of~Db~AM~FRa~Ac~A9fun~ARI got to~A5~BI~Ay crimping~F2s~Akroubleshooting/updating~ANrs.~CCwas work~AAI~EFed~DX~IUI believe~AAmy~HKty of~Ee ~Bswork~AM~FRa was bas~FNmy decisi~C4~HGr for ~Ik '97. A~A9my~GU follow~APose event~CVbas~FNmy~Hve~JJe to~HKties.~Am heard of an a~DV~AAadmitted academic~FFrs.~Grw~Bmoment I heard~ABa~DV~AAthis was~BG~AA~Gl~AJjoin. Along~Bvay, my~Bgt in t~Ajcontinu~AJgrow.~Ab 3.2 - Math Tutor At~C7, I~HWed in~A2l.~AF far behin~AWrest~B4~ALmates~AR~Ana~A9catch~F6~D6. My m~BDwas look~Eaideas~AA~A6help me. E~CWy, she hired~FS, who~Acn except~Ccmath tutor.~Fc week, ~Ge~AlI~AC go~Cjmy~Ehhomework~AA~AGfalling behind on. He~Acb~Cyhelp me to~Bhan~AW~DI~AA~AGnot grasp~ED~AL~CN~BNded reviewing what~AF taught in~AL,~EV ove~BVhomework~ARpracticing~Banal math~B1s~F1 I fully~Bhood them. B~Ecend~AB~A3 year,~AF ~Bsgo on~ASnext Algebra~Ffe. I~A_no idea~A7I~AC~A_done w~I0 him. I owe~Army~GU to him~Ako my m~BDfor~Ee ~Bsfind him~Cftalk~BU~FS~Ay my~Bgt in t~Aqy, I mentioned~AAI~A0heard~Ay a~AK a~AT~A3~BZt~AVmy. He~Gumed me~AAhe knew abou~AT~AXan~AWt~Dds~AAworked in~AAp~Ai. He ~DPt~AAit~AC be a~Hn~AK~EYe~ARso he wrote a ~Ij of recommendation~AS~AXtell~APem~AAI~AC be a~Fkfit. I~AH~Ij~AAhe wrote~ASA~DV, he indicate~AWamou~FDacademic~Afess~Am made~AMhim~CQ~AIplann~AJdo once I~FJed~As. He~CFgave~Gncopy~ABlette~DNhande~AW~BDcopy~ASa~DV~EOwas considering who they~AC be admitt~ED~Ag~AK fo~BVnext ~IG. I~B3~GF my~EC ability, my~B0 Mr. Puzo, show~Cuhow I~A1 look a~ATwhole wor~Dyno~AT~Dhpart~ABword~Beread~BRsentence. For~EP:~FXt~DRMr. Puzo' s advice, I~AC see~HFe a~BQword sen,~Dw ignor~APe rest~ABword, tence~CfI~DTMr. Puzo's advice, I~CR~AJfocus o~AHword as a whole:~HFe~F4~Ck this, I~HP~AA~AG~Bsread~BI~Cifluidl~Diquickly. Ad~CZally,~Am a~EI time~AMmath~CA~AM~Gb~AF ge~Fqbette~DN~IN. E~CWy, I~AC enhance my~Cw~Dx~Alpass all the~Cqd~Ehcourses~BEn to math,~AF~Ejg~Ar~BDsubjects~BH~Et, ~H3y, science~ARPE.~AF~A0tak~BRcoup~Fzel~Ggs as well,~BH c~Ei, engineering~ARart~DKel~Gg~ALes~AC~Cz~Dxs~AAwere~CBfor a~E5r~DK~D5~AC~BNde~Btread dir~Ek~Alpresen~AToutcome~A4the direc~Chin a~E2ive~CX.~Ab 3.3 - El~Ggs One~ABactivities i~AHengineering~F5ive~Gdd a silk~IR us~AJprint t-shirts. Any art~F3n~A1 be scanned~Akhen sprayed onto a piece of silk~AA~ADthen be laid~Cja t-shirt~AMblack ink~EG~AJthe top. In this~AL, I attempt~AJsca~AHcover~B4 C++~AKming book~AAcame~AM~EJ~HAo and~Gm~AAlogo onto a t-shirt~BEn~ASsilk~IR~Gming,~B5~AY~IF~BDactivities as well. One~AEese~Gdd us~BRmetal ball~G9sca~Cymeasur~BKimpact~AAit~EWwhe~AHball ~I2 down a ramp; an~BDactivity~Gdd~Bhand~APe flight mechanics of an airplane~Ip it~Acn~Bgting~AL,~CI some~ABsame t~Ajfrom~Axech~AL in~Gke~A2l to~Cz~Bmaterial,~BH a VHS player~DL~AJa TV. I~A0took an art~AL~Ca~At~DW~Cqd sculpt~BRcereal box~C- us~Bya marketing~F3n.~Am an~Bgt in runn~Eo own b~Cxa~AT~EKand~AF go~BU us~BKlast ~Ij~Dt name, 0, a~BQlogo.~Am come up~AMthe idea~A4RTSOFT,~AUompany~AAdevelope~AWBBS game, Legend~ABRed Dragon. I use~AWO logo in~BI~AEings~BH b~Cxcards~AA~Anprinted up. In my sculpting~F3n, I~F3ned an O~C9fict~Cccereal ~Hty~BZ"O-flakes." This~BMhe~C7 time~AAI~A1 see a~Exbit~B4 corporate branding~Ee~C5even ~DP~Atroduc~BAfictional. A~ATvery end~B4 biology~AL,~AF given a~E3 assignment ~CEI~AC grow ~JK inside a petri dish.~AF paired~AManother~AQ to form a team~ARthe~AHt~Dd handed~D8am a petri dish~AMa yellow substance o~AHinsid~B_a Q-tip. My team~BMhen ask~AJtake sampl~Fy~JK~A4objects~Dathe~ALroom us~APe Q-tip~CQip~BKQ-tip~AMthe ~JK on~Ag inside~ABpetri dish~FXplacing it in~Ag oven. As a team, my~E3 sepa~IOour jobs~CGtwo roles, the~AQ~Dt~E3~AC be~JJibl~D7collect~APe ~JK~ARthen I~AC be in charge~ABoven. I~AHoven~B8~JK~AC grow~Cjtime~ARwe~AYe~CWy ~Bsvisualize it. My team~A5ed~BI from~Au~Bo~BH~Btcommunicate eff~Ggly and~Btdivide our tasks~CN~AC~FPe an~In~Bce~AAI~AC use~G2 in~Ad~Ip I~EFe~AWlab and~Ee ~Bs~JQe as a~E3.~Ab 3.4 - BBS Hobby O~Bxmy hobbies I ~Ixed~CGwas dialing~CGBulletin Board S~F9s (BBS). These~AY~C1s run by hobbyists~AA~A6be accessed~CI a~ANr~C3 telephone line. A b~Fhd ~C1 or BBS is a~ANr~Fvr running s~HRized~Fse~AeBBS~AC~G3 a ~E1 who~AC call in us~BRmodem to direct chat~AM~BDlogged-in users,~ETpublic~Ew boards, exchanging~Ews~AM~BDusers,~ETbulletins,~ETnew~Alupload/download~Fs~B_data. A user~AC~DL~AS~C1 us~BRterminal~AK. (B~Fhd ~C1~C8~Ae~E1 call~EDto~Aaystem was~DA~AWuser~ARthe ~E1 runn~APe ~C1 was~DA~AWSystem Operator or SysOp. Users~AC~DL in~Agse ~C1s~CI a~ANr~AMa modem attached~AAwas~CFconnect~AJa phone line. O~AH~BDend~ABphone line~B5~AC be a ~Iv~C1~AMan almost identical setup~Aewhole ~C1~A4~EBs end~ASSysOp~Acrranged~BXa~Gvt~Cterver~Bi~Aeuser~AC run a simple text-based~AK~AAwas capab~Fzm~DRcalls~Ao a modem attach~AJthe~ANr us~BRserial~F2 ( a type of data~F2),~Dw \ ~Aw----- the SysOp~AC run a BBS~Fse~AK~AAwas capab~Fz~Eling calls. Whe~AHuser~DA~AWBBS us~APe terminal~AK~B8BBS~AC send text~ASuser~AeBBS~AC usually~CR by show~BRlogin form~Aelogin fom1~AC~GG_ ques~Ch~F-~BQuser's nam~B_age~Aeform~AC~CFask the us~CPpick a usemam~B_password~Aeusemame~AC us~Dlbe an alias~AAdefined who ~EB was but not~DF actual given name~Aeusemame~A1 be a~JHe c~HC~A4a novel or~Bq~BDalias~AA~A6not be identified~AMthe ~E1's real name~Aeusemame~AC~CFbe~H0shed o~AHBBS. Other users~AAc~Cgin~Ag BBS~AC~CFsee~Atrevious user's usemame who had~BZbefore. A BBS ope~IOmuch~BXa b~Fhd on a~Ad campus. Other users~A1 pin notes on~Ag b~Fhd,~FM~A1 be~ETby anyone call~APe BBS. Tra~CZ~DSBBS~GGed~Ew areas, fil~G8s~ARgames. Messag~G8s~AY~Ivto an online forum~AM~Ev subforums dedicat~AJa topic. A user~AC post to one~ABsubforums~AR~BDusers~DL~AJthe BBS~AC be ~Bsrea~Dymake comments or replies to~AAforum post. An~EP~GAtopic~A1 be selling~C5items. Similarly,~BSil~G8~AC be a~GN wher~BKSysOp~A1 be~GNs so it~A1~G2 be download~AJ~EB's~ANr~Hwtimes~D4 fil~G8s~AC be~BZ"CD areas" becaus~BKSysOp~AC purchase a CD fu~Cdshareware~DKCDs~AYmanufactured specifically for BBS and~GGed~Fjdex file~AMall~ABshareware title~Aldescriptions. BBS~Fse~A1 rea~AW~ISfil~B_displa~Ec~ISi~AHfil~G8. Ad~CZally,~BSiles o~AHCD~AYcompressed usually~CI zip-file compression so~AA~EB~EEneed~AJdownload one file i~B3 ~El~BKfull c~IIs~ABshareware~AK~Ez a game or utility. A known b~Fhd ~C1~Fvr~EGc~AZdevelop~HqMustang S~FUin 1986 for MS-DOS and~G2 port~AJ~EJ~Gcwas~BZ~JX. (~JX 2910)~AeBBS~Fs~BTquite intricate, ~ELing~Arconfigur~AZto get it to run~GQonce it had~FQset up properly, it~A1~C_calls~Aoou~ATda~Dinight. Calls com~ED~Ag BBS s~IV~BI~BXa fax call com~ED~Aemodem~AC~CFtak~BKanalog call~Almodulate~Aaignal~CGdata~AA~A6then be~ETb~EcBBS~AeBBS~Ac~Hn way to talk to~Cp~E6 town~Ay~BWthing~A4t~Ajto~Hb~DHy~AYa welcoming~GN~AAallowed anyone who~BZin to~A_an~Hv~Ib i~AHconversa~DfE~CWy I became~Bgt~Bystart~Eo own BBS. My inspir~AZfor want~BU~CR a BBS was my~Bgt i~AHt~Aqy. So~Fw a~Dw, I studie~AWt~Ajand ~Ille~AWs~FUnecessa~ILru~AHBBS~G7 AST 486~ANr. I~DA~AWBBS "The Quendor BBS" based o~AHuniverse~E2ed~DaZork. My BBS~CO small~Ew area, a CD/Fil~G8 fu~Cdshareware f i le~Ala Door/Game s~Ek~BH L.O.R.D. (Legend~ABRed Dragon)~E2~By1989 by Robinson T~Aqies, an online textbased role-pl~Givideo game (Legend_of_the_Red_Dragon~BfE~CWy, I~A5ed~Ay ~I5t,~AAwas a worldwide~ANr~Bi~G3ing b~Fhd ~C1s to ~El~B_send~GD~BCs to ~BDb~Fhd ~C1s o~AHtelephone~Bi. (~I5t~Bf~I5t~AC sit in fro~FDa BBS and mak~B_~Ele calls~H-~Dpe BBS us~BRset of ~I5t~AKs~CN~Bi of BBS~G3ed~Ews~C- sent~I1s state~Alcountries. ~I5t was~AUlosest ~BBto an~Bnnet~AAwe~CO~ATtime.~Am~EGed fo~BVnode, 1 :202/315~Dkad set up a few~Ew areas~DL~AJ~I5t. A~ATtime,~AF ~El~BRfew calls~A4users~AAhad~ETmy advertisement~C9local magazine.~AF~A0read~Crposting~Ews ~E6~AUountry. E~CWy, I~AC join meets ~CESysOps~AC talk~Ay each~Bl's ~C1s. We~AYalso heavily marketing our BBS~CI leaflet~Alfloppies~GGing~Fse~AAallowed users to call our BBS. I~AHend, I met~Argreat ~D3~AMa mutual~Bgt i~AHhobby. I~A0l~Cl~BI~Ay se~Fqup a ~C1~AAacted~BI~BXa web~Fvr. As a~GDc~AZ~C1 for its ~EKBBS~CO distinct advantage~Cjin-~E1 convers~C0. For~GE who may~A_~Ihe gett~APeir ~DPts ~Iu, BBS~FEd~Fjfinite amou~FDti~Bpplan wha~ATuser want~AJsay~Aemessages~AYalso prerecorded,~G3~APe us~CPgo~Cjtheir own past convers~C0~Ef ~DP~AF a quiet ~E1 a~ATtime,~AF~G4 sociable o~AHmessag~G8s. I ra~AHQuendor BBS for~Ay four~HQ~F1 I closed it down in 2001~Fw my~ANr suffered a hard drive crash.~Am~A5ed~BI~Aoou~ATyears. I~A5ed~AAit takes~Arpersistence se~Fqup a ~C1~CQa~D2for users to call in. I~A0l~Cl abou~AT~D5 one needs to customize a BBS. Ever~DG~A4the login ~IR~ASmenus~A1 be customized~CI text files. I~A5ed~BI~Ay turning ASCII c~HCs~CGartwork~Dkad~Arfun~Dw~DX it.~Ab 3.5 - C~Ei O~Bxmy~C7~F5ives~AAI~DTin ninth~F8~Ac c~Ei~AL. S~Azs~AC cook ~G_ meals~CI recipes~A4a cookbook~Cfa coup~Fzweeks, the~AQs broke~CGgroups. My~E3~AC~CFjoin at a small kitchen space~AAwas~Fxbig~Gjh~Egpproximately five~AQ~DHre~Ac sink, cupboards fu~Cddishes, cup~Alc~Ei ware, an oven~ARa stove. Each of us~AC~C_a role. I chose to b~BKdishwasher~CN~Ac relaxing occupation~AAdidn't~Cq~Areffort and~AAanyone~A1 do. In a way, I turned dishwashing~CGa~EQ ~CEI~AC~C_any dirty dish and~H5 begin to clean it. It ca~Bpa~DM~Ca~AF cleaning dishes long afte~BVmeal was~FGd~AR~Edst~B4~E8was~EF~APe meal. Fin~DSafter~Am cleane~AWdishes, I sat~AMmy~E8and~EFe~AWmeal~AAmy~E8had~FGd~DKmeals~BNded spaghetti, salads, pizza~AR~BDdelicious foods. E~CWy, I~AC~FPe frantic, look~Eaany dish~AAI~A1 find~Ctpend~BRlong~Bucleaning~F1~BW~BBwas spotless~Bknt~AJshow~AA~AGthe dishwasher and~AAno one~A1~C_my spot. In reality, ~DP, I~CYwant~D8am ~Dms~Fmn~G5I~CYknow an~DG~Ay c~Ei.~Am see~AHrecipes~AAmy~E8had~FQread~AP~Bdan~AW~Cot measurements they~AYl~C2g~Ay. I feared~AAif I tri~AJhelp~D8am, I~AC botch~BWthing. Then, o~Hx, one~ABgirls on~D8am said~AAshe~AC do~Bwishwash~Ded~AAI~AC cook. I~Frnervou~Alsaid~AAI~A1 clean~AR~Edst~B4~E8seem~AJagree~Dvshe insisted~IUso~AAon~BP I~DTo~AHnew job. Most~AB~EKI~AC observe~D8am ~Dms to~Bhand~A7they~AYdoing~Dw avoid~De~DG~AA~Gq complicated. It seems so funny~BeI~FB~Ay thos~BP~Al~IZe~AAthe meals~AAwe~AYc~Ei~AYincredibly simple.~Fc~BBhad~C-~FGd within a two-hour~Buframe~A4start~Fmnish. If~Am~CO ~Go~Ci~Fac~B_had focused~Cion~A7my~E8~Dms~AYdoing, I~FB I~AC~A_known~A7to do. But~AF honestly afraid~AAI~AC do some~BBto rui~AHmeal~Dje are~Arfactors involv~Byc~Ei. And~BeI'm alone, I can~He all~AEose tasks~AMease. But~B5 are~CDs~AAne~AJb~BPd~Akhings~AAne~AJbe~Fdin order. I'm sure~BWone has burned a meal once~C9while or dropped mix all over~BSloor. So~Hgs do~F7n. I~AHend, I~A5ed~BI~A4the~AL~ARat least one~E8~Dm cared~CQant~AJsee me do~Cithan simply wash dishes. A~ATend~ABclass,~Ax~Dd gave us a final athome~CX~Dqa~DfI~Dq~AJthe~AL a~Iziption~ABmeal~AAI~FGd at home. I~BAmy~JHe meal~AAm~CS~AC~FOall~Axime: tuna and mayo sandwiches~AMsweet pickles~D0the~AL wa~GMas impressed~AMmy~Dqation,~Ck feel proud~B4~Hjhment.~CCwas my~JBre effort to~Dq ~Ag~AL w~AIenjoyed having at home.~Ab 3.6 - Keyboarding~Itrs in~As~AYa relatively new~CD~AAwe~HS~E9 to~BW~AQ. We~CYhave~ANrs in~BW~ALroom~ARwe~AYlucky some~ABclassrooms had~ANrs at all. So in tenth~F8~BeI saw the~FWding~F5ive~AL, I~EU~AJtake it~F4 I en~I2,~Ck so~Eu~AF ~G0~Bgted in~ANrs. Being ~Bstype well~CYcross my mind. The~AL utilized a~AK~G9~Gc3 .11~ANr~AA~ADprovide tr~HO o~AHhome row key~DH~AK~AC ask ~Bppres~BQkeys as fast~C3s accurately as I~A1~ARthen I~AC~D_ a score bas~FNhow well~Ck. The~AK felt~BXa competitive game, except~AA~AGpl~Giagainst the~ANr~ARI~A1 go at my own pace.~Fc day, I'd play~Axyping game hop~BU~GF to my next ~IP level. E~CWy~C3fter~HVs of tr~HO, I manag~AJ~Do the~AK.~Am gone~Ao~BW key us~APe home row keys~CI both~B4 hands.~Am mastered ~IP~Ae~IP~AL was by far my~JHe~AL an~AWmost useful~F5ive~AAI~DTin~As. I~CY~IZe ~JG~AAclass~AC come~CGplay~F1 I~CL ~IP~H2s in~Ad. I don't~D9~JG~Buit~AC~A_taken ~Bptype an~H2 if~Am not mastered ~IP. An~H2~AAshould~A_taken thirty minutes~A1~A_end~Ght~DRhours if~Am~FQforc~AJguess wher~BKkeys were~Ef~DX~HHh o~AH~AO~Cqs ~IP. I wonder why~HDary~A2ls do not offer ~IP~Ffes~Cfall,~AM~AOs i~AHclassroom, I~AC~FB~AAthe~A2ls~AC wa~FLoffer~IT~ANr~ALes as ~I9as possible so~AAthe~AQs can~C_full advantage~ABt~Aqy~D0I know~AAsome~HDary~A2ls do offer a dedicated~ANr~AL, most~ABtime it's up~ASteach~CPtrai~AHs~Azs in~ANr~EG~BC~Alkeyboarding. I~AHend, I am grateful to~A_ha~AWopportunity to~A_taken a~FWding~AL. About ten~HQ ago, I~EU~AJretrain~Erf on a~CT one-handed~FWd us~BRMaltron. It~DTme~Ay a~HV to lear~AHnew~FWd layout. So~AAtells me~AAeven ~DP I've had~Artrying~Bces, it~Ep doe~GMtake~AAlong to~A5 a new~FWd arrangement. Since not~BWone ha~BQopportunity to~C_a~FWding~AL in~As, most~Ads offer general~FWding~ALes~ARthere's~A0lots of good~FWding~AKs online. Al~DP, I~Ep~FB~AAit's a~Fkidea to~DJe primary~A2l~Hf~Btus~BKkeyboard,~JB most~EMis now~ANr based.~Ab 3. 7 - A~DV~AF overjoye~AW~HU I~HP out~AAt~AVmy had~D_~AW~Ij of recommend~AZby ~Ge~Alhad accepted my~EG~BC.~Grw~AAthis was~Axurning~DM~AF hop~Eaand~AAI~AC be one step closer~ASsuccessful life~Am~ERdreamed~Ay. T~AVmy was~F3n~AJhelp~AQs~FG for~Ad~Aea~DV comprised three~ALes: American~Ise, US~Hbal/~H3y~ARa~Ffe on~ANr~EG~BCs. I~AHAmerican~Ise~AL, I~AC~ET~IFpopular~GK~Ay America i~AH~I9to late 1800s to 1900~Alwrite a~Hpt~Ay them~BEn to~E3~ECs, I~AC~A0present my~Hpt ~Ag~AL.~Am studied my~Dqation, but~Beit cam~BP for ~Bpspeak, I fumbled~Ao the entire~CD~Dje~AYsome good~Dqers i~AHclass~ARthen~B5~AYsome~AAdidn't try. H~Bzlater on in~Ad, I~AC co~Bp-..~EF~Dqing. It~Ac time~BeI~A1 show off w~AIresearched. I~AHUS~Hbal/~H3y~AL I~A5ed~Ay our government an~AWUS Constitu~DfTher~BTone~DM at~FM~Ax~Dd held a mock trial~Aetrial incorpo~IOall~AUomponents~GAUS courtroom~BNd~APe court~Hpter an~AWjury.~AF acting a~BQbailiff~ARi~BAmy~JJibility to keep~AUourt in order. Mr. Allen, my US~Hbal/~H3y~AL~B0, always~CO~E2ive way of runn~APings. F~Fe,~B5~BMhe~ANr~EG~BCs~AL.~CCwas~AUJass~AAI~EFe~AWmost. The~AL~Gdd~A5ing~Ay office~EG~BCs o~AH~AO~ARthere~Aclso a~E8aspect to it. T~AVmy~A0~Eq us~FG for~Aaenior~CX in twelfth~F8. In eleventh~F8~B8a~DV held its own exclusive version~ABsenior~CX.~CUred~AAmy~CX~Gdd a large sheet of cardboard~AMstring~DLing~HDs~AAwere glued on. T~AVmy~Ac big help~Beit~EXto~A7I~AC ne~AJknow once I entered~Ad. I~BA~G0~F3ned as a~Ad ~GL~AZa~DV~Dvit~A1~A_~Eq any~AQ~AAwanted a head~CR~Fw~As. T~BLout~DQ as a~AQ in~HDary~A2l~Akhen~Gke~A2l, I've~CO few~Bgts~AAI've want~AJpursue as a~E5r. I~AHthird~F8,~AF~Bgt~Bybecom~BRdet~Gg. I us~AJplay~Bwet~Gg role~AMmy ~IA gear pretend~BU listen~Ao wall~Alwr~D2code words. By~Axime I~A5ed~Ay~AKming, my~Bgts had~IMed~Hu. I dreamed~Ay~Db~AM~AO~Al~Csat Microsoft~Bknt~AJwork as a~ANr~AKmer~F3ning b~Cxappli~BCs. T~AVmy played a hug~Ib in advanc~Eo~Bgts in t~Aqy. T~AVmy~BMhe only~AK at my~As~AAwas~Czing~ANr-related~EG~BCs~BEn~B8a~DV~Aclso ~GLing~AQs for~Ad. I owe~BI~ASa~DV~AAhelp~Cubuil~AW~D5~AAI~AC~D1for~Ad along~AMMr. Allen, my ~H3y~B0, who~Aclways motiv~Fp~Bptry harder. As a whole,~BWone i~AHa~DV boosted each~Bl's~FZnce. We all knew~AAwe~AYi~AHa~DV~Eu we~AY~HI~AJsucceed.~CC~Eq ~Bp~GJe~GU in areas~AA~AGstruggl~ED, mainly~E3~EC. During~E3~ECs, I~CYfeel as nervous as~Ck i~AHother~ALes. While~AF a proficient reader a~AT~EKI often~AC run~CGsome~ABtypical~B1s~AAmany~Bl~AQs~AMAVPD had. For~EP, ifl saw~Bvord "t~BL," I might~A_pronounce~AWword as "~DPt." Typic~DSwhen~AF~EC~C9group,~Bvords o~AHpage~AC~FPe blurred~Ef ~DP I~A_20/20 vision, I~AC feel like~AF~HWing~AMread~APe text o~AHpage. But~Eu~BWone i~AHclas~CV~HIed,~BWone~Aclso respectful of one another.~CCwa~GMa ti~Bpact immatur~B_laugh or giggle~Dvrather it~Ac ti~Bpgive our best to~E4~BDand act properly as teens who~AYbecoming mature~GOs.~CCmature atmosphere e~CWy~C6~Bpregain~FZnce~Dtself. I~CYfeel ashamed~AMmy~EC~Dra~AT~EKand~AF grad~Dlbuild~F6 my~Dxs.~CCmade me want~C- an~Hv~Ib~ABgroup. I~Dzstrong~CQas no longer scar~AJspeak in fro~FDan aud~Dnbecause~Am~Bce~AMpublic speaking. My~H0c speaking~Bce i~AH~AXstart~AJbuild my~FZnce. All~AEis~DTtime~ARat~C7 I~AC feel~G4 nervous~Dvall~AEis~FAaway~BeI~CR~AJspeak~AS~E0~CEspe~DRin fro~FDan aud~Dnbecame natural~EYe~Aea~DV~DTus~G9~JR~ASMuseum~ABH~Gf in Los Angeles,~E-a~F4 we arrived, we~A5ed abou~ATgenocide of European Jews during World War II~Aemuseum was~HP~Hqa~E3 of h~Gf survivors at Hollywood High S~Bjwho met in 1961~Aegroup ~IZed~AAthey~CO deep~DLion~ASHolocaus~ENbegan ~Cso~AHmuseum~EOi~BQoldest museum~HP~Hqa h~Gf survivor. (Los Angeles Museum~ABH~Gf~BfThis close-up~Bce of~Ee ~Bssee images~ABvictims,~DF living situ~C0~ARthe torture devices~C5on them was horrific. Nothing~AAI~ETat~A2l~AC~A_compar~AJthose pictures:~Dushowe~AWbeds~AAthe victims~AYsleep~Ds and bodies stacked upon bodies~Aevisual~Ew~Ee~CX~AJus i~AHMuseum~ABH~Gf a~HS unlike w~AIhave seen~A42019 ~CEimmigrants~AYbeing lock~Ghin detention~Imrs~AeNew York Times article~Izibe~AWdetention~Imrs locat~ByClint, Texas, as be~De overcrowded border station~AAwas filthy, sickening~ARchaotic. Young~Cpwho had crosse~AWborder~Aveir hundreds~AYbeing held~B5. All~AEis~Gum~AZwas accord~BU lawyers who had visite~AWfacility~Hw~AB~GW~AYbeing~Hpted as having been~B5 for n~I9a~HV. (There Is a Stench': Soiled Clothe~AlNo Baths for Migrant Children at a Texas Center~C8~Aewa~Ec~Fgs is now tre~Fp~D3~Avese camps is~Fxas horrific a~BQwa~EcNazis treate~AWJews dur~APe H~Gf.~CCi~BQreason why I believe in~A8. W~AIsaw~CFchanged how I~Bnpre~ATworld~Dame. H~JL visite~AWMuseum~ABH~Gf, I~AC~A_never~Bhood ~JG suffering had occurred due~ASNazi regime dur~APos~BPs. Because I know~AAthis can~F7n, I'm~A0aware~AAthis can~F7n any~CEat any time~ARtherefore~BeI~FOpolitical~GXns~Ez who I~FKvote for, I~A_a ~IN sense as to who I~AC not wa~FLvote for~CQhy. People may see detention~Imrs as a means of controlling immigrants~DvI~Bhand~AAno~BBgood can come~A4those~Imrs if~Cpare forc~AJbe~Avem~Hust~DF will~EEbecause~Dua~HS a citizen~AEis country.~Ab 3.8 - Appearance O~Hx, I~D_d~Fjvit~AZfrom~EZ to attend a news broadcast~AA~Gy~BU~F7n i~AHlibrary. He mentioned~AAI~FKdress up~F4 I asked how, he said,~C9suit~Akie. I spoke~AMmy m~BDabou~ATfilm crew and~A7~FRa told me. She~CFsuggested~AAwe~FKgo to JCPenne~Dipick out a suit~Akie~F4 I return~AJ~A3,~EZ ~DPt I~Eyp~CMl~ARwe met i~AHlibrary wher~BKfilm crew was do~APeir~Hpt~Aefilm crew~AYthere~Hpting o~AHMac~ANrs an~AWn~DC~AAwas built during ~Ik '97~D0they~AYfilm~APeir story, I stood by one~ABreporters who ~DPt~AA~AGone~ABstaff ~Dms. He~Eysurprised~BeI said~AA~AGone~ABs~Azs a~AT~A3~Aedress~FHblended in~AMthe ~BDadults i~AHroom~ARno one~Act all suspicious. I~A_to admit~AAI~Eygood~AAday~ARthe film crew ~DPt so as well. A~ATend~ABday,~GBt~BXa new ~E1, as if~Am sh~Empast self~IUyet~Auglimmer of hop~BTsome~BBI held ~C4knowing~AAeven ~DP~AFn't out~ABforest, I~Ep~CO long way~Fo~F1 I~EWit~ASend. High~A2ls in Southern~E-a~A_study~ALes dedicat~AJs~Azs who~D1a~Exextra help.~AF in one~AEose~ALes. Unfortunately, most~AQs a~ATtime did not~Bhand why a~AQ~AC~D1a dedicated study~AL. Those~AQs may~A_~DPt this~Ac~AL for under~GJer~DHr~BTon~BP~Bea~AQ w~GCover~A4t~AVmy~Ctaid to me, "Aren't you ~IN~HNthis?" My heart sank. It~Ac direct blow~ASone~CD~AA~AGtry~BU~IC~ARthe~Afess~AAhad I~EWinstantly left me~CA~AAbrief mome~FD~Galike~Am disappointed~Erf e~CWy lifted~Ef ~DP ~Gled~Bqextra~Dca~ATtime,~AF~Ep~DX~BI~Cito~IC~Erf. On top of keep~F6~AMmy general~A8,~AF~A0doing~BI to~FG~Erf~EYy~E5r.~Ab 3.9 - C~A- I~Cv~AA~Fb~AJlearn more~Ay~AKming. Until~Aupoint,~Am not~FQformally taught.~Am~A5ed~Cbo~AK~Ao m~CS's ~Ii~Ak~BL the help~B4 t~Ajt~Dd~DvI~F0~DTa~AL on~AKming~FCday,~AF~G7 BBS~ARI posted a~Ew i~AHmessag~G8 asking~BWone how I~A1~A5 more~Ay~AKming~Hwone replied~AAI~FKlook~CGmy local~GDty~Ad. I asked my~A2l counselo~DNmy~Ehtutor,~FS, how I~A1~C_a~AL and discovered~AAhigh~A2l~AQs~A1~C_~Idn~ALes over~Aaummer. I~Eyat all~ABavailable~ALe~Aldecid~AJtak~BKmost~EI~AL I~A1 find, CIC++ Object Oriented P~Aiming.~Am~A5ed~Ay C++ o~AHBBS and felt~AAi~BAthe~AKming~FYe~AAmost p~CMl~AKmers~AYusing. Before the~AL~CL, ~Ge~AlI attempt~AJ~Ill ~EJ~HAo Integ~IODevelopment E~Ir~EGc~AZon my~ANr. ~EJhad~E2e~AW~EJ~HAo~EOis~Fjtegrated~ESment e~Ir (IDE) to~G3~Cpto~ES~ANr~AKs~BEn~B8IDE~A0allows~Cpto~E2e~Ess, web apps, web~FTs~ARmobile apps. (~EJ~HAo~BfEven ~DP we~AY~Bs~Ill ~EJ~HAo~G7~AN~DNru~AHappli~BC, we~AYnot ~Bscompile any~ABappli~BCs. Puzzled, I ~DPt~AAmaybe by~Axime the~AL~CL I~AC be ~Bs~Ic out wha~ATp~DO was. O~AH~Dhday of~Ad,~AF ~GL~AJgo~Ao~Bmost~EI~AL~Am ever taken.~AF in a~ALroom fu~Cdp~CMl~GO~AQs~AAwere~B5 for ~IFreasons~BH~Ad credit.~AF only~Avere so I~A1 call~Erf a C++~AKmer.~Grw~AAall~ABp~CMl~AKmers~Bhood C++.~Am~ETbook~Almagazines earlier~AA~ADtalk~Ay C++~Dje~AYcode samples~AMC++,~Cpt~Gp abou~AT~H3y of C++~ARp~DWs~Ca~BW~BBwas focus~FNC++. As I'm writ~APis, I feel as if I am pick~F6 the j~I3 and visualiz~APese articles. Dr. Dobb 's J~I3~BMhe name, and~AKming was my dream. UBM T~AjGroup~H0she~AWmonthly magazine, Dr. Dobb 's J~I3 (DDJ), i~AH~Fgs~Aemagazine covered topics targeted at~ANr~AKmers. (Dr. Dobb's J~I3~BfI~AC read~Auj~I3~BW~Bua new issue~EXout, marveling~Cjall~AB~Go detail~Alp~DWs. Dur~APe~C7 week,~Ck not know~A7to~I_t. The~AL was no~BBlike~As.~B6l~Ei a~ATsyllabus~Akhinking~AAthis was~BG~AAI~A1 do.~AFn't worrying either, but~AF~A0not~FGd for~A7I~AC face. Almost~H5, I ~IZed~AAI di~DUknow~Btcompile the~AKs. If~B6~Igtly,~At~DO~DBimple~Dvbeing~DJely new to~Cw C++~AKs,~AFn't ~Bs~Ic it out. I~A0~A6not explai~AHp~DO~AF having~ASp~DYor, as I~FHdi~DUknow~Btexplain it or explain why~AF in an~GFd~AKming~AL~C-gin with.~Fcone else appear~AJbe well above my level~ARI seem~AJb~BKodd one out~Aeonly ~BBI~A1~FB~Ay was note~Ejg~ARsomeday~D4 notes~AC e~CWy~Dcme to~A5~C- a C++~AKmer~Aep~DYor had notice~AWpages upon pag~Fynotes~AF~Ejg.~AF~G4 literally copying down~BW word~AAhe said and~BW word on his slides. He must~A_~DPt~AF crazy.~Am no idea if an~DG~AA~AGturn~ED ~Gyod~Dvl~Ei over~A7everyone els~BTsubmitting~EWme feel~BXw~AIhad~BMrash. I~AHend,~Ck not~D9what~AF~DX~ARhalfway~Ao the~AL, a~DZ me felt~BXI~FKquit,~Dw an~BDpart of me~Dzlike~Auwas my~EEhope. But~AF glad~AAI took~AUhallenge, - - ~Aw~Aw--~CH did stay~F1 the end~CN~BMruly an~Bce worth having~Dje's a~GH ~Coce~H-~Ee a~AQ in~As and~Ee a~AQ in~Ad~D0~Ihemakers can exist in~Ad,~BI~AEose types a~HS go~BU stick~AMit if~Dudon't~A_to go to~A2l.~B6on~BP in~As when~AF si~Fqdown,~Bqkids~EU~AJthrow trash at me. I~AHend, I pick~Ghmy~CD~Alw~GCaway to avoi~AWsitua~DfI'm sure~AAI did immature~B2as a~As -s~Az, not~ASextent~AA~AGthrowing trash, but~B2like~AAdo not~F7n in~Ad. So~BeI w~GCin~Ag~AKming~AL, I~Dzlike~AF w~Gp~CGa mature~GO e~Ir. Upon my return back to~As i~AHfall, I saw~B2in a new light.~Grw~AAmy senior ~IG~Acbout mak~BR~Coce in~DQ.~AF boun~Dy~HI~AJtry harder than~Am tried~Fne. But~B2start~AJcatch up to me~Hu as~GB~ATweight~ABfinal upcoming~HVs of~As. One~AB~DI~AF excited~Ay was~Aaenior final~CX.~GBt~BX~Fb~AJprove to my~B0s ~JG~AF able to~A5~Ay who ~Fb~AJbe.~Fc~CEI went, I~AC~HM "~H7C++ Object Oriented P~Aimer."~Ck~Aubecause my~Hnest fear~AC be~AAI~AC~FPe stuck in lowskilled, low-p~Gijob~Ie meant~AA~Anto b~BKbest~AAI~A1 be. Unlike~Bl~AQs,~Ck not~A_any room for~I4.~Ab 3.10 - Resume T~AVmy~A0~Eq me~AM~H4~Cw and~Bnviewing~Dxs~Ae~H4~AA~AGcre~Fpmatched~AAof a general sample~AAI~A1 find today o~AHinternet or~C9job-hunt~ED~G6~Ccbook. All~ABfeatures~AB~H4~AYclosely identical. H~Bzconsidering~AAthe~Bnne~BAquite new back then~AR~Annever seen a ~H4 in~G6ion book~Fne, fu~Cdsample ~H4~Alinstruc~Chon~A7to~BNde~C9~H4~AA~ADbe us~AJapply~DE~HZ at a ~Hty. I may~A_never ~Icd out~AAI~CBa ~H4 i~B3 obtain a quality job~Ae~H4~BNded all~ABbasic i~BO~AA~ADbe~HP in any~H1l ~H4~BH~Dxs,~A8~ARemployment~BEn to~Cw my ~H4 , I~A0acquired~Bnviewing~Dxs.~Fcone i~AHclass~BMold~AAwe need~AJgive a firm handshake~Bewe met ~D3~Cfthat, we all lin~Ghoutside the~AL~ARone by one,~BWone w~GCup to our~B0 and gave~DF best handshake. F~Fe,~AMthe~GT we gained, t~AVmy~DTus~G9~JR~DEcareer fair~Ca~B5~AY~G_ ~Hties hiring~Aejob~HKties~BNded~BWthing~A4cook~BU office assistant-type work.~B6meeting some~AB~Hty representatives, hand~APem my ~H4 ~ARthen tell~APem a ~Go~Ay~Erf. I~AHend,~Am gained universally~EGcable~DrI~A1 use in any situa~DfT~AVmy~A0taught us~Ay~Ad.~B6o~Hx I~FAon a ~JR~AMthe a~DV to visit one~ABlarger u~CKies.~CCtrip~BNded see~APe~IE and~ALrooms i~B3 get a feel for~A7I~AC~Bce~G2 on~BeI~EGed~BEn~AS~JR, I~A0l~Cl~Ay apply~BU~Ad~Alu~CKies~CN~BNded how~EGc~AZprocess~GPd,~Cbransferring credits~A4one~Ad to an~BDworked~ARso on~Ip~D4~Drhelp~AJ~GLe me for~A7I~AC ne~AJdo next~DKare~Aakills~AAevery~AQ should~D9before leaving~As i~B3 ~GJ~BKbest~E5r prospects possible. I feel~Enthankful for hav~APe~HKty to~Bce t~AVmy~Ako~A_gaine~AWknowledge and~Bce~AAwas provid~AJme. A lot~AEis~AC~A_been a mystery h~JL gone~ASA~DV.~Ab 3.11 - P~DW While~AF l~Ei over~Aaenior final~CX~Cqments~Am~CL ~Cson~AAsummer, one~AB~ELements was~AAI~CBa mentor. I ~DPt~Ay it~Eg~Dw~AMmy~Ehtutor,~FS~ARthen we~Cv~AAI~FKreach out to P~DYor Trinh from~Ad. Mr. Trinh agre~AJbe my mentor~ARthen we plann~AJmeet at his house. P~DYor Trinh~Iy by helping me~Bhand some~ABp~DOs~Am in his~AL at~Ad. We~CFwent over~Aaenior final~CX~ARhe suggested~AAI~FKdo a~CX o~AHYear 2000 bug (Y2K). P~Aimers us~AJus~BKlast two digits~C9year to ~JYce~Bewrit~APeir~AKs. A~ATtime whe~AHp~Ais~AYbeing~GV,~Atric~D7memory was expensive,~A4as low as $10 per kilobyte to in many cases as much or even~Cithan US$ l 00 per kilobyte~IUi~B3 keep~AUost down on both mainframe~ANrs (and~G2, ~E1al~ANrs) storage~HTy was~C5sparingly. (Year 2000 P~DO~BfEssentially,~AKmers~AC ~JYce by not us~APe~C7 two digits~C9year~Aep~DO, h~Bzwas~AAonce 1999 was up,~Bwate~AAthe~ANr~AC see~AC be 19100~Ae100 came~A4the~ANr add~Dse to 99~Ae19~F0~IM~AJ~JYce. P~DYor Trinh~CH t~GCabou~AT~JA~IHr~E2~HqJulius Caesar. Greek mathematician~AlGreek astronomers~EXup~AMa~IHr~IKpt, - --- - ~Aw based o~AHproposed idea by Julius Caesar~DA~AW"~JA~IHr,"~FM~AC~CR on 1 January 709 AUC ( 45 BC) by edict~Aecalendar~AC be a reworking~ABRoman~IHr. (~JA~IHr~BfP~DYor Trinh~A0showed me~Bqof his~Bl~AKs,~BH a menu-driven~AK~GV in~IW~Cfwr~D2some~AKs in C++ and~HHh~APe~B1~AMthe Y2K bug, I~F3ned a form~AAran~C9console window~Aeform~AC~C_input~A4~EB~Cttore~AAinform~AZin a file~AA~A6later be retrieved~Aep~DW demonst~IOhow a~AK~A1 be Y2K compliant~GQin actuality,~AF ~Iqing~AA~Anl~Cl~Cbo~AK in C++.~CCwas my m~H8 in life:~C-come an~Bced~AKmer us~APe same~AKming~FYe~AAthe p~CMls use.~CCwas my passion and~DQ's~GYge, and~AF f~Fe ~Bs~GJe it.~Ab 3.12 - M~H8~AF proud~ABaccomplishments~AA~Anmade up~Apis~DM.~AF able to~Bce~Ad for~BSirst time.~AF ~Bsfulfill my dream and love for~AKming.~Am~AUhance to work~AM~FRa~ARmost~Inly,~AF~DX well~Dt math~ALes thanks to~FS, who help~Cuenroll i~AHBusines~AlT~AjA~DV~AAchanged~DQ. M~CS~Ac hard~GPr~Ef~Aoou~ATnight sh~BTwork~Crrarely had fre~BP. She~AC~ERtry to~FOfre~BP~Fo~AMme on trips. We~AC~C_va~BCs~I1s~E-a,~Ao Oregon~ARwe~E7visited ~EJin Seattle, Washington,~Ca~Am a ~If~G1~BKlobby area. We've taken dozens of trips~ASskiing resorts in Big Bear M~Gtn,~E-a. Along~AMmy~Flmother, we've innertubed~Ao the rivers near ~JA and boated~I1~BQbay~AMmy~E_. M~CS~Ac~GH part~B4~A8~ARI~AC~F0 be ~CEI am now if it hadn't~FQfor her. Right~Fn~BKend of~As, I~DTthe ACT at a private u~CKy. I~AH~Fgs,~AU~A- admissions~EQ~Cqs a standardized test~DA~AWACT (/er si: ti:/; orig~Fe an abbrevi~AZof American C~A- Testing). (ACT~BfMy results~AYnot n~I9what~AF hoping for. I~Ep had~BI~Cito~A5. Al~DP~Ck dwell~G7 failure, I~A0understood why I~FFd those results. L~Ei at~Au~Bo now, I'm glad~AAI~FFd those result~DHr~BTso much~Dt~A8~AAI~Bhoo~Dyyet~E7more~AAwas m1ssmg. Before~FJing~As, my~B0, Mr. Puzo, my~HL~AR~Ges~C6lead me i~AHdir~Ek~AAI~AC~C_once I reached~Ad. Along~AMm~CS, we drove to my local~GDty~Ad~ARfrom~B5~AU~A- took~Cjand show~Cuw~AIneed~AJdo i~B3 apply.~Fcon~BTextra helpful in~DMing me i~AHright direc~DfBut~F1 I~CR~AJmeet~AMc~A-~HLs,~Ck not know~A7to~I_t. At~C7, I ~DPt~AAI~AC~H5~CR t~DRIT~ALes i~B3 ~El~BKnecessary tr~HO~EYy~E5r as a~ANr~AKmer; h~BzI then~Bhood~AAI~AC need~Ar~HB~AZfirst~Aepoint~ABgeneral~A8~ALes~BMo~FOme a~Ciwell-r~IV ~E1. H~BzI~CY~G0 know~A7to~I_t~F1 my~C7 year at~Ad,~FM~AC not~F7n~Eg ~Go~Dw longer~Aehigh~A2l held an assembly for~Axwelfth-grade~AQs~AAwent ove~BVdetails~Ay~Ad. A~ATassembly, I~A5ed wha~ATrates~AYon~Ad dropouts. Each~ABs~Azs a~ATassembly~Acsk~AJlook towar~AW~E1 on our~Hr~Cfwe~Io we~AYthen give~AHdropout ratio.~CCis~BG~AAalways stuck~AMme.~Ck not want~C- one~AEose~AQs who droppe~G5of~Ad, leaving all~AEeir~Afess behind. I~AC ne~AJ~Gb persistence no matter~A7and keep~EV no matter how~EI the~ALes seem~AJbe. To be fair, ~DP, eve~AH~Dh~IG of~Ad~EWa huge ~Coce-I~A5ed~BI~D0it~AC~A_been a disappointme~FLdrop out~Fw six~HVs, this~AC~A_still~FQ~IN~HNnot~EV at all. It~AC~A_increas~Em~Ifs of~DJing~Ad~G2 on. Thankfully, none of~AAhappened~ARI ended up~DJ~Eo~DD~B-. As~Aa~Bjyear ca~Bpan end,~B5~Ac ~If I'd~A_to~C_more~As~ALes over~Aaummer~Akhen miss gradua~DfThat, h~Bzdi~DUhappen.~Am~DJed all~B4~ALes on time~ARI~AC~FJe~AMa~HY at hand.~AF proud~B4self~AReveryon~BTeq~Dlproud seeing me a~ATceremony. ~Ge~Alhis wife~AYthere.~EZ~BMhere~ARall~B4~B0s, my family~ARm~CS's ~Iis, Ms. Winstanle~DiMs. Altman,~AYalso~B5. They all watch~Cuas~AF hand~Emdiploma by Mr. Peck,~Atrincipal~AB~A3~Aehigh~A2l~Bce~EWme ~HI~AJsucceed.~AF~Ci~Fat~HNever, and~AF ready for~AUhallenges~AAlay ahead of me.~AF~A5ing~Btpush~Erf forward to~GF my~A8~ARI~AC not let anyone or any~BBstop me.~AF a~ATpoint~Ca~AF~ES~Eo p~CMlism o~AHway~C-com~BRmature~GO.~Am~A0developed aspira~Chto~FPe a~E5r~AKmer.~Fc~BB~E6 me in~As~C6~Bp~GJe my goals~Hz ~HGr~Ea~Ik '97 to join~APe ~AXand t~DRmy~C7~Ad~AL~Ie summer, I~DTon~ANr-related jobs.~AF~CI~BI~AB~D5~Am attained~Dw in~As. Back then, technical ~Ia~BAnot as readily~E9 as it is now, so it wasn't as uncomm~C4seek~Dcif~GE tech savvy lived i~AH~JU. Today~AMthe dozens of~E9 IT ~Iat op~Ch(at least aroun~AWbig cities), it's~BI easier~Eg new~AQ to get~CR~ByIT by apply~BU an IT firm o~BVlocal~ANr store. A~A9my IT~GT~EXfrom~Ax~Aqy~AA~AGl~C2g~Ay in~As.~Am~GPd a~AT~A3 ~Illing~EG~BC~Aln~DCing. I~A0knew~BI~Ay Windows~EG~BCs~A4the B&T a~DV.~Am PC hardware~Bce~A4when I~AC upgrad~BKmemory~G7~ANr~C3dd~De~ICd modem card onto my BBS. I~A0had my own thinnet~Bi at home~AAI us~AJtransfer files back and forth~Aen~DC~C5thin coaxial~F2 terminated~AMBNC~DLors to~Hy a local area~Bi~FM was~BZ1 0BASE2 (~A0known as cheapemet, thin Ethernet, thinnet~ARthinwire ). (10BASE2~C8.~Ab 4 - C~A- I~B3~GF my~E5r, I~Cv I~AC~D1more~A2ling. I~FAback~AScommunity~Ad~C3pplied for~BSall ~IG~Bknt~AJ~GJe my goal and~FPe a~ANr~AKmer~F4 I return~AJthe~Ad, I discovered~AAI~AC~D1a~ANr sc~Dn~B-. W~AIdidn't~D9a~ATtime was~AAI~AC~A0ne~AJtake~Armath~ALes. Nonetheless,~Am done~BI of~AKming i~AHpast.~Am~E2ed~BWthing~A4a text~EQing~AK~DEtelephone~AK. I mad~BKdecision~AAsummer~AAI~AC apply at my~GDty~Ad fo~BV~EA of sc~Dn~B-. Ad~CZ~DSI~EGed for~Banal~FTs for ~D3~BXme who~A_a d~CJy~Cftaking~Bqtests a~ATc~A-,~BH tests~AA~ADcheck my~HTy~Dxs,~AF shown~AA~Anscored high on~Bqareas. My long-term~HTy~DBurprisingly good; h~Bzmy short-term~HTy was lacking. Know~APis~Gum~AZhelp~Cuto~Bhand my~HXh~Alweaknesses~ARI've~A5ed~Btadapt ~Agse ~Coces. Being new to~Ad,~Ck not know~A7to~I_t. H~Bzw~AIdid~D9was~AAc~A-~AC~FPe my new~JJibility~DKstrength~Alweaknesses are~A7I've~A5~AJlive with.~AF a~AQ in~Ad~BXany~Bl~AQ, fully capab~Fzsucceeding~Fxlike~BWone else. Like~BW~Bl~AQ attending~Ad,~B5~AC be numerous~GYges along~Bvay~Bknt~AJbe~GUful.~Grw~AA~AGmissing critical~Drneed~AJget me~Ao.~Am~Cnre~AWday~Bemy~As told us abou~ATpercentage of~Ad~AQs~AA~ADfail.~AF~Fud not~C-come a ~Ivstatistic. So I~CL~AMthe~HEs~AA~AGgood at and~AU~A-~ALes~AAwere recommend~AJme.~Ab 4.1 - L~BrS~HhMost~ABtime ~Gl~AJgo~Cja~B1~Dp~BPs i~B3~Bhand it. After~Aaecond or third~Buread~APe~B1~B8question~AC seem much~CT~HNit sounde~AWfirst~BuI~ETit. Also,~Beit ca~Bpessays, it ~Eq~BI~Besomeone else~ETw~AIhad~GV to catch any rem~HO~I4s. Often I~AC~IMe a word~C9sentence not ~IZing~AA~Anto rea~AWwhole~HFe~Cjagain to~FOsure~AAit~Ep~EWsense; h~Bzeven~BeI~Io sometimes I misse~AWmistake two or thre~BP~DHre~AYtimes~Dt math~AL~BeI'd get stuck~ARthe easiest ~JE~AC be to~CR over~A4the~En~H9g. Calculators~AYnever~G3~Bya math~AL, so~BW~BBhad~C-~Fdon paper~BEn,~BW textbook~AA~Ancontained~Fjdex and glossary s~Ek~Ae~ISs~Ek~GGed keywords~AA~ADreference a page i~AHbook~Aeglossary~GGed definitions~ABwords i~AHbook.~Fc~AL~AAI~DT~EL~BFse s~Eks~ABbook. By us~APe ~ISand glossary s~Eks, I~AC sav~BP look~Eakeywords or ~BDinform~AZin~D8xtbook. I~A0used highlighter pens to~FO~Idn~HFes~AAI ~DPt~AYimportant stand out~A4~Edst~CN~AC sav~BP~Betry~BU find a s~Ek~C9textbook~AAwas~In~Hw~B4 textbooks~AYover a thousand pages long. It~A0~Eq ~Bptake a mix of~ALes: ones~AAwere~EI and ones~AAI knew I~A1 do well in. At on~BP,~Am two~Et~ALes~AAI~A1~A_taken a~ATsam~BP; i~IJ, for~EP, I~DTa~ANr~AL and one~Et~AL ~Iu, so I~AC only~A_one~AL~AA~Gl~AJfocus on~Hw ~BDgeneral tips~AAI'd follow~BNded meeting~AMa~HL~BW semest~CPmake sure~AF on track. If~Am~EU~AJchoose the~ALes~G7 own, I~A1~A_ended up~Ejg~Arunnecessary~ALe~DHse are some~ABmore common strategies~AAI us~AJhelp me~Ao~Ad. One~ABlifestyle rules I mad~D7myself~BMo abstain~A4alcohol and parties. To me, alcohol was~EEa~B1 for~GE who a~JC~CO weakness. My short-term~HTy~B1s~AYa~JC bad~Gjh. I~CYneed an~BDp~DO~Bknt~AJget my~A8, and~AA~AD~ELe a sober mind. It~Ac goal~AAI~AC~Hse fo~BVrest~B4 life. I~AC avoid ~Ih~B_an~DG~AA~A6cause it. I~AC distance~Erf~A4everyone.~Am to~E7consider~Axhought~AAm~DR~Iis~A1 lead to ~Ihe.~AF~IKrned~Ay~Ee~HP out, and~AF~IKrned~AAwhat~F7ned in~As~A1~F7n~Hu.~Ab 4.2 -~Et The very~C7~Ad~ALes I en~I2 in~Ac~IT~Et~AL. The~AL was packed~AMs~Azs. In this~AL, we~A5ed~Ay~IT gramma~DNpunctu~AZrules. We~AYl~Brabou~ATuse of apostrophes, commas, semicolons~AR~BDpunctu~AZmark~DHre~AYa~A9grammatical rules~AA~AGnot familiar~AMinclud~APe use of apostrophes to form a contr~IQ, for~EP, shorten~APe words "do not" to "don't." We~AYalso taugh~AT~Coce betwee~AHwords~DF,~Du're~ARthere in~Ban to ~BDhomophones. C~A- was paced so~AAprogressing~ASnext~AL~Cqd~Hf~BW~BBfrom~Atrevious~AL. Before I~A1 enroll~Dt~Et~Ise~ALes, I~AC ne~AJunderstan~AWbasics of~Et. C~A-~Acbout perfect~Eo~Drso~AAevery~HD of~Cw~AC be~Bhood~Ie meant~AAthere~A1 be no run-on~HFes or grammatical~I4s any~CEin my~Cw. I~E7had to~FOsure~AAmy~Cw~DBophisticated~Gjh to not sound bland~CN~BNded us~BRvaried mix of words so~AAevery paragraph s~IV stylistically unique. E~CWy, my~Cw~CR~AJimprove.~AF m~DRfewer~Hgs~ARthe flow~B4~Cw s~IV~Cipleasant~ASreader. I~A1~EEguess~A7my~Cw style~AC~A_~Gq~BXh~JL gone to~Ad. My~Cw~Dxs~AC~A_been severely lacking~Ef writ~APis book~AC~A_been~GYging, maybe~E7impossible. My emails~AC~A_been fu~Cdgrammar gaffe~Alspelling~I4s,~FM~AC~A_compromis~Emintegrity. I~AC~A_never recogniz~BFse~I4s h~JL~COn~A8~BEn to~A5ing spell~Crgrammar, I~A0need~AJlearn~Btcite ~Gzs ~Igtly. In~Ad,~B5~AYtwo cit~AZformats used, AP A and MLA. C~A-~Ffes in~Et studies, modern~FYe~Alliteratures, comparative~Ise, literary criticism, media studies, cultural studies~ARrelated disciplines use MLA document~AZstyle in scholarship. (MLA Handbook~BfAll ofmy papers~Cqd ~Bpcite ~Gzs~A4~Cot texts as part~ABresearch. Sources~A1 come~A4~Cot~GK from~AU~A-~IE or a site~BZEBSCO Host. Many academic, medical, K-12,~H0c~IE, law, corporate~ARgovernment markets use a~IE re~Gz~BZEBSCO. (EBSCO ~Gw~AZS~F9s~BfUsu~DSI~AC~A_to~BNde ten cit~C0~A4three~CT ~Gzs in~Ad~CN~A1~BNde a few ~Gzs~A4~Ev~Es~Al~Ev~GK as long as~AF~CI at least three~CT~GK and/or three~CT~Ess~F4 I~CL a~ATu~CKy,~AUit~AZ~ELement~FAup to twenty cit~C0~A4ten~CT ~Gzs. Since c~D2~Gzs~Ac relatively new ~BBfor me, one~ABvery~C7~GK~AAI purchased~Ac~Ad MLA cit~AZreferenc~Fi~Aereferenc~Fi~BNded each~ABformats~AAwere us~AJcite ~Gzs~Aecit~AZi~BO~AC~BNde~Gum~AZ~F-~BQwebsite URL,~Bwate~BeI retrieve~AWtext,~Axitle i~BO~ARa paraphrased or quoted line~A4the~Es. A reference page was~BNded a~ATend~B4~Ad~H2~AAalso~BNded~Aucit~AZi~BO.~Ab 4.3 - C~EH I desperately want~AJwork as a~ANr~AKmer~Dw attending~Ad. I~CR~AJlook~Ao my op~Chon w~AI~A6do~D0thumb~APrough~AUourse catalog, I discovered an a~GIte's ~B-~AK in~ANr science. H~BzI~AC~A_had to take~Banal~ALes~AAI di~DUne~AJtransf~CPa u~CKy. I~CFcame~I1s the~ANr~AKming~D-.~CCc~EH~AC deviate~Erf~A4my original planned goal of earn~BR~EA's ~B-~DvI~A0felt~AAthe devi~AZwas worth the effort. I~CR~AJtake~AUourses I~AC need~B9~AKming~D-~Dw t~DRmy ~HB~AZcourses. Knowing~AAI~AC~Hse my dream of~FPing a~ANr~AKmer gave me a~GZ of hope. Because~AEis~GXn, I~CR~AJthink~Ay~Ad as an ~GJable feat.~Ab 4.4 - Tu~I8 While~Ad may~A_seemed~BXan uphill climb,~B5~Aclso a vast amou~FD~Iat op~Chavailable~BNd~APe tu~I8~Imr.~Grw~AAI~A1 always~C_my~H2s~AStu~I8 cent~CPgo~Cjmy~Hgs. Visit~APe tu~I8~FT~Aclmost a~Cqment~Egll~B4~Et, math~ARforeign~FYe~ALes. I can't coun~ATamou~FDtimes~Besome~BBI wrote s~IV perfect to me~EElat~CPfin~G5from~Axu~I8~FT~AAmy paragraphs~AYway too long, my cit~C0~AYin~Igt, or my commas~AYi~AHwrong~GN ( or~B5~AYa lack of commas al~Iu).~Ab 4.5 - Frustration~AF runn~EDto~Arp~DOs~Beit ca~Bpmy~Ehwork. A~ATvery ~H9g~AB~IG,~AF ge~Fqfrustrated~AMmy math~ALes~AS~E0~CE~Fb~AJfocus~G7~Bl~ALes~C7. Unfortunately,~AAfrustr~AZ~Hcd~ARI e~CWy ran out of ti~Bpfinish all~ABmath~Ffes~AA~ADbe~Cqd~B9~ANr sc~Dnmajor a~ATu~CKy~BEn, I~HP out~AAI~A1~A_tested out~ABmath~ALes~AA~AGfrust~IOwith. So~AMthat, my dream of~FPing a~ANr~AKmer~AMa~ANr sc~Dn~B- ca~Bpan end~D0my~ANr sc~Dnpath ended,~AF~Ep~Hs~APe~ANr~AKming~D-~Cfa~Dw,~Am built up~Gjh~AKming~Bce~AA~AGeven~DX~AKming o~AHside. Fin~DSafter~HVs of hard work,~Am f~Fe earn~BF~ANr~AKming~D-.~AF overjoyed~AAall my~A2lwork had paid off. Gett~APis~D- ~I9on ~Gy~BU~Dcme. While~AFn't ~Bsget a~ANr~AKming job~AMthe~D-, I~Ep~Acb~Cyuse it to show~AAI had~ANr~GT~ARI~A1 use it to compete~AMsomeone~AAdid not~A_any~D-s. It~A0help~Cuby giving~Gnbit~GAmoral boost. At~Aupoint,~AF~HW~Cr~Galike~AF i~AHmiddle~ABocean~Cttill had~HVs~F1 I~AC see~Aahore. With the~AKming~D-, I~Dzlike~AF o~AHlast mile~B4 journey~CA~AF facing yet an~BDp~DO.~AFn't sure w~AIwant~AJdo next. At one~DM I switch~AJa b~Cxmajor; h~Bzit wasn't~BG~AAI~EFed, so I~EU~AJfind some~BBelse. I went~AScareer~Imr at my~Ad ~CEthey gave me~Bqtests.~Ab 4.6 - Assessment The~E5r assessment~Iy by find~Eo~HXhs~CI an online~E5r aptitude test~CN~BNded finding out if~AF~Cilogical or physical~Aetest~CFstart~AJgo further~CGmy~HXhs~Fmn~G5what~AF most~Bgt~Bydoing as~DZ a daily routine. Once I~DJe~AWtest, I~FA~C4an~BDone~AA~Gq further~CGmy specific~Bgts~Aeques~Chon~Autest asked, for~EP, if I~AC wa~FLgive orders ot solve a~B1. A~ATend,~Axwo tests~AC give~Gnscore.~B6~El~Eo result~Ali~AHnumber-one spot of~E5rs~AAI~AC be best suited for was~AKming.~AF relieve~Dydisappointed a~ATsame a~ATtime~CA~AAwasn't~Gjh to stop me~A4going forward. I~FAback~AScareer~Im~DNdecid~AJlook a~AT~IFbook~Alwebsites~AAthey offered. One~ABbook~CVcalle~AWOccupat~Cc~Gx. ~Gw~AZabou~ATnature of work, ~Cscon~CZs, train~Ded~A8, ~C2gs~ARjob outlook for hundreds of~CT occupa~Chi~AH~Fgs is~H0sh~Bya book~DA~AWOccupat~Cc~Gx b~Ec~Fgs Departme~FDLabor's Bureau of Labor Statistics (Occupat~Cc~Gx~BfI read~Ao th~Fi~Ctcanne~AWwebsites l~Ei for~BG~AAinterest~Cuor~AAI~AC be~Fkat~GQI~A0was l~Ei for~BG~AAI~AC be secure in. I ~Gq~Cjsites~BXmonster.com~G1e~A7high-demand jobs~AYpopp~F6~Hw~AEese jobs~BNded ~Gw~AZT~Ajconsultant, IT manager, web~ESer,~ANr consultant, ~C1s analyst, ~C1s administrator~ARn~DC s~HRist. These~AYhigh-p~Gijobs~ARthere~Acn abundance of open ~HZ~DHre~AYalso~Arhigh-paying~ANr~AKming jobs~Ez~EG~BC~AKmer~Ctystem~AKmer~Hz the~En~H9g, I assumed~AAI~EEhad one option i~B3 earn an IT related ~B-. In retrospect, I~CYeven~FB~AAmaybe I should~A_done~Bqmore~HHh~G1e~A7~B-s~AYavailable. Now my only~B1 was~AA~AGstill~G9single pathway~DE~B-. I~CYknow if~B5~AYany ~BDsuitable pathways~E9.~Ab 4. 7 - A~GIte's At~Au~E0in~DQ,~Am~Cv~AAI~AC not~Hse a b~Cx~B-. I ~DPt~Ay w~AI~A6do~ARI spoke~AMmy~HL. We~Cv~AAI~FKsee ifl~A1 earn an a~GIte's ~B- i~IJ. I~HP~AA~Anenough~Ffework~AR~Ana~JC~DJ~Em~HB~AZ~ELements. My~HL~CH~DTall~ABwork~AA~Andon~B_combin~BFm all~CGan a~GIte's ~B-~Cfthat, I told my family~ARI attend~Emgradu~AZceremony~CN~Acn exc~D2part~B4 life, as~Am some~BBto show fo~BVamou~FDwork~Am~DJed up~Apis~DM. I took~BW~AL~AAI ~DP~BAimpossibl~B_claw~Emway~Ao them~ASend. As I~DJed one~Ffe, I~AC be given an~BDcourse~ARthis~FAon and on~Ae~Gawas~F0 ending, but~BeI~DJ~Emlast~Cqd~AL,~AF relieved.~Am gained~BI~A4my educ~AZove~BVyears. At one~DM,~AF~Cw~H2s~AMhundreds of~Hgs~AAwere both confus~Crdifficult to~Bhand. C~A-~C6me~AMmy~Cw~Dras well as~AMall~B4 ~BD~HB~AZ~D5. By~FPing~Ad e~IYd,~AF~A0becoming~Ci~Fat~Dtself.~AF~CR~BU see~Atossibiliti~Fyw~AI~A6~GJe.~AF~GZ~Cic~II at~Au~E0knowing~AF do~BRgreat job~AMmy~A8. I~Ep had mor~BP and desperately want~AJ~GJe a~DD~B-. So I stayed at~Ad~Fxa ~Go~Dw long~CPfin~G5w~AIwant~AJdo next.~Ab 4.8 - Decision My~HL mentioned a private u~CKy her daughter~Acttending~Aeu~CKy~Ac fast-paced~Ad~AAhad fourweek~ALes rather~HN~IG~ALes. What caught my eye~BMhe amou~FDt~Aj~B-s~E9. Until~Aupoint,~AF~Ejg~BWthing~A4political sc~Dnto music~Fxto try~Fmnd some~BBI~AC~EF. These~AY~B- pathways~AAdid not~Cq any~Ehand~A1 be~Fdin~BI less time. H~BzI~A0understood~AAthese ~B-s~AYnot meant~EYe and~AAthey~AC~C_me i~AHwrong direc~DfI~Cv~AAthe ~Gw~AZT~AjM~Dg~B-~BMhe~Hr choic~D7me~Ef ~DP i~BAnot a~AKming ~B-, I~AC~Ep be~DX~BWthing~AA~Anbeen~DX in terms of~Db~AMt~Aqy. I~A1~A_asked~Erf why ~Fb~AJkeep~EV. Why did I wa~FLpursue a~DD~B-? After all,~Am~Fx~Cl an a~GIte' s. Wasn't~AAgood~Gjh? Ever~DG~AA~AGdo~F6~Apis poin~BAwork~BUward my passi~C4work in IT as a~AKmer~DvI~A0knew my weaknesses. Simply put,~AMa~DD~B-, I~AC feel~Cisecure in a~AKming posi~DfW~I0 a ~B-,~GBt~BXanyone~A1 replace me. Being~C9~HZ of employment~Hoprivilege, not a~Hr. And~AAprivilege is~Fud by~AUompany, no~ATemployee. Now was~Axi~Bpsolidify my spot~Bknt~AJmake ~Idn~AAI had~AUredentials~AA~ADmake me equal among the ~BDcandidates. A~DD~B-~AC do~Fxthat. It~AC give my~H_rs~FZnce~AAI knew~Btdo my job.~Grw~AAthis was~Axi~Bp~Do my~A8~IUso I w~GCinto~B7~Ctaid~AAI~AC~BXto apply as a~AQ.~Ab 5 - U~CKy By~Axime I~EWit to~B7,~AF exhausted.~Am half~B4 educ~AZleft~ARthe last half~AC be relat~AJmy~E5r. I met~AMan advisor who~AC work~AM~Bpsee~A7~B- I~AC be~Bgt~Bypursuing~Hz the list of ~B-s, I narrow~Emchoices to ~Gw~AZT~AjM~Dgand~Itr ~Gw~AZSystems. I~Cv o~AH~Gw~AZT~AjM~Dg~B-~Eu~AF a~JC familiar~AMsome~ABcourses~A4the jobs~AA~AGdoing~ARI~CYne~AJexperiment~AMsomething~AA~AG~Doly unfamiliar with. I~A0di~DUne~AJrepea~ATwork~Am~Fdso far in~Ad. I~EEwante~AW~B-~Ako finish my~A8. So I~EU~AJtak~BK~Gw~AZT~AjM~Dgroute. It~Acct~Dla good~CD~AAI~Io too,~JB~Ad was getting~Enexpensive~Hz here, I~A0decid~AJvisi~ATcareer~Imr.~AF primarily~Bgt~Byknowing if I~A1 get a well-p~Gijob~AMthis ~B-. I've~A5ed~A4talk~BU ~D3~AAunless you're highly intelligen~ENyou~A_unique~Dx~Ala~A9money,~EV~Fw fancy ~B-s~AAa~HS high in demand i~GMa~Fkidea~D0I do~Bhand~AAeveryone wants~D6~BG~AAthey~EF, it~A0makes~Ftal sense~D6~BG~AAis in demand as well. Actually,~ANr~AKming~Acnd~Ep is in demand~ARit's a shame~AAI wasn't will~BU risk~Hsing~AApath~JB at~AAtime I~A1~A_done it~GQa~ATsam~BP,~Bwegree~AAI picked~Aclso in high demand~ARI knew~AAI~A1 do it~AMno~B1. ~Gw~AZT~AjM~Dgultimately~Frmy pick.~Ab 5.1 - Starting Over O~AH~Dhweek of March, I head~AJmy very~C7 u~CKy~AL. At~Aupoint,~Am~Gjh~Bce go~BU~Ad~AAI knew what~AF gett~EDto. As~Am a seat,~At~DYor~CL~EV~Cjhis slides. Just~BXmy very~C7~Ad~AL, I~CR~AJtake notes on~BW~BBI saw~CA~Autime I~Bhood what~AC be~I_ted of me.~B7~Hop~CMl u~CKy~AMp~CMl-type ~D3,~Bqof whom~CO~JC~CR~BFir~E5r~DH~ALes run for five hours~BW ~BDnight~ARthey last four weeks.~B7 is~A0an accredited u~CKy~AAhas~Aaame standards as ~BD~Ivaccredited u~CKies.~Fc ~BDday, I~AC~C_notes~ARthe~AHp~DYor~AC test us on~A7we~A5ed dur~APe secon~Dylast week~ABclass~BEn, this~AL~CO~CX~AA~ELed~HHh~BRtopic~CQrit~De~H2 on it~AA~ADthen be~Dq~AJthe~AL. We ha~AWopti~C4work in teams~ARthi~CVrecommended~CN~AL~Cqd more~CDs~A4me~HNin any~B4 previous~Ad~ALes~Dva~ATsam~BP,~Eu I picked a major~AAwas a~JC part~B4~E5r, I stood a~Fk~If of succeeding~Avis~AL~F4~AF in~As,~Ck not .,~Bhand why ~Gl~AJknow algebra or any~B4~Bl~As~HEs~Aeonly~CD~AAkept me~EV was~Hf~AA~A3~AC lead ~Bpa~ANr~AKming ~HZ~Cfcomplet~Eo u~CKy ~B-, I~A1~Bhand how my~As and~Ad~Ffework~C6~Bpbecome a~GUful~AQ.~AF~FZnt~Ay my~H2s.~Grw~Btpresent them ~Ag~AL~ARI~A0understood w~AIneed~AJdo if I~CBhelp. My~A2ling had essentially ~GL~Cufor~BSinal two~HQ~B4 life a~ATu~CKy. W~AIgained~A4my~GD~BCs~ALes ~GL~Cufor~Atresent~C0~AAI~AC do dur~Eo u~CKy year~Almy~E5r. Becoming proficient in~Cw~G3ed ~Bpwrite~D8n- to twenty-page~H2s~BW~HV~Aearts, science~AR~H3y~ALes~EWme a~Ciwellr~IV ~E1 so~AAI~AC see~CDs~A4new persp~Ggs~D0math~BMhe most~EI~HE~EYe a~AT~EKit~A0taught me~Btsolve~B1~Alnot to give up.~Fc single~CD~AAI~A5ed in~A2l and~Ad~C6~Bpget~Ao~ASvery end. It~Acs if~Atuzzle pieces~AYf~Fe com~BUgether. Dur~APe~C7 year at~B7,~AF~Ejg~Artechnical and business-related~ALes, from~ANr hardware to~ANr s~FUand~Biing. All~B4 p~DYors~AYvery ~Iative~ARif~Am a~IKrn, they'd listen. Whil~BKwork was~GYging, it~Aclso some~BBI~A1 wrap my mind ~E6~Dje~AYa lot~AEings~Am~GPd~AMbefore~ARa lot~AEings I've~F0 known~Ay, ~F-~BQ~GcRegistry. ~EJ~Gcoper~Fp~C1~ARsome~EG~BCs us~BK~GcRegist~ILstore low-level settings~C9hierarchical d~IXe. (~GcRegistry~C8~Aep~DYors~C5~Ev methods of~Cz~APe c~II ~Ag~AQs~Aoout the~AL~DKmethods~BNded hands-on~Hvities w~BYe~AC work~AMthe hardware or~EGc~AZto~A5~Btmanag~B_configur~BKdevice. We~A0performed our own~HHh by~EC book~Alarticles o~AHt~Aqy~Dje~Ac ~EJOffice productivity~AL~AA~ELed hours upon hours of enter~Crdesigning ~IFWord, Excel~ARPowerPoint documents~CI~BW single feature~Avese~EG~BCs~Aen~DCing~AL~Cqd an~Bhanding~AB~H3y of~Biing~A4the ~JDz~C0~AAcame up~AMthe specifi~BCs to specific protocol~Alhow~Duworked. The~AL was~ITally a condensed version~ABCCNA in a~ALroom format. Cisco Systems~FEs~Fjform~AZt~Aj(IT) certifi~BC~AAis~DZ its a~GIte-level Cisco Career certific~AZcalle~AWCCNA (Cisco Certified N~DC A~GIte). (CCNA~BfEach~AL~A0~ELed a~HHh paper~DKpapers~BNded~BWthing~A4a topic on ISDN and raid levels to b~Cxstudies~G9~Idn t~Ajbeing~C5~F-s~CI Bluetooth to track ~GW~Daa theme park. Fin~DSeach~E3 of~AQs~AC~Dq~DF~HHh~CI PowerPoint to~Dq~DF findings ~Ag~AL. Dur~Eo second year at~B7,~AF~A5ing~Ay IT securit~Did~IXes. I~AHd~IXe~Ffe, I~A5ed~Ay d~IXe~H1liza~DfI~B3 reduce data redundanc~Diimprove data integrity data is ~G6ur~Bya relat~Ccd~IXe in accordance~AMa seri~Fyso-c~Cgnormal forms~BZD~IXe~H1liza~Df(D~IXe~H1liz~AZ2019)~Hz~B5, I~A5~AJdesign~Dpe d~IXes~CI MySQL and ~EJAccess~AeIT security~AL~BNded~BWthing~A4l~C2g~Ay fire suppression ~C1s~D6or lock~Al~Cot encryption methods for both militar~Dicivilian use. Each~AL was lai~G5i~AHsame fashion~A4my previous year's~ALes ~CEI~A1 either~EMin teams or individ~Dlto go~Cja topic. For~EP,~Dt~HlLAN~AL, I talked~Ay war driving~EOinvolved driving~Da~JUs~Dw~CI s~HRized s~FUon a~ANr to capture~Hlpackets~ARhow ~D3~AYcollecting data o~AH~Hty's wireless~Bis. I~A0talked~Ay directory access protocols~Dt Linux~AL.~Ab 5.2 - NUAITP One evening, two i~Hals~FVup during o~Bxmy~ALes at~B7. They~AYt~Gp~Ay a~AQ~Iw~DA~AWNat~CcU~CKy Associ~AZoflnform~AZT~AjP~CMls or AITP~AeAssoci~AZof ~Gw~AZT~AjP~CMls (AITP)~Hop~CMl a~GItion~AAemphasizes~Gum~AZt~Ajeduc~AZfor business~AQ~Alworkers. (Associ~AZof ~Gw~AZT~AjP~CMls~BfMr. De~DiP~DYor Bugado talked~Ay~Cbheir~AQ~Iw~A1~Dcthe~AQs~Dt~AL and~AAi~BApart~GAlarger ~JDz~AZc~CgAITP. I~EU~AJjoi~AHchapter~Eu~GBt~AAit~AC give~Gnsense of belonging~AS~A3. Dur~Eo time~AMAITP, the~AQ~Iw~FAover~Ar~Cot typ~Fyt~Aqy. P~DYor Day invited us~DEt~Aqy-oriented event w~BYe~A5ed~Ay~CT t~Aj~Hties. I met~ArIT p~CMls~A4the NUAITP~ARwe~FAon to~A5~BI~Ay~A7each of us~AYdoing~AMour~E5rs~CfI~FJed~A4the u~CKy, Mr. Bugado~CH~FAon a recruitment trip ~E6~AUampus~ARwe passe~AWtorch~ASnext~E3 of~AQs who~AC take~Cjand lea~AWNUAITP~Iw~Ip it~Ac~FHgood~Bce~AR~Ana ~If to meet~Bqgreat~Cpand~A5~Aramazing facts~Ay IT.~Ab 5.3 - Capstone One~ABvery last~ALes I~DTat~B7 was~AUapstone~CN~AL~Ac~CX m~Dgcourse ~CEthe~AQs i~AHclass~E2ed a ~JE~Eg real~Gvt~AMa real b~Cxneed~Hw~AEese~CXs~BNded modify~APe~Gvt's existing~Bi infra~G6ure or ~Ill~BRserv~CPsolve a~B1.~Fc part~ABp~DW we~AY~Cson was based o~AHPMP, a p~CMl~F3n~AZrecognized worldwide offered b~EcP~DW M~DgInstitute (PMI) (P~DW M~DgP~CMl~BfThe~AQs,~BH~Erf,~AYdivided~CGgroups. My~E8set out to solve a~B1~Egn ~JDzation~AAneeded a~Es. Our~E3 met~AMthe clie~FL~HIe wha~AT~JDz~AZneeds~AYand~Cbhose needs~AC fit~AM~Edst~AB~JDza~DfFrom~B5, our~E3~Fud~AAa CMS~AC b~BKbest ~JE fo~BV~JDza~DfSoftware~EG~BC~AAcan be us~AJmanage~AUre~AZand modific~AZof digital c~II are~BZc~II m~Dg~C1s or CMS. (C~II m~Dg~C1~BfMy team~CF~Ille~AWCMS on top~GAweb-hosting~FEr~Es~Akhen set up a backup ~JE in case~B5~Ac disturbance i~AHwebsite hosting. Fin~DSour team~AKmed all~AB~JDzation's~Gum~AZin~Ag websit~B_provided tr~HO~AScustomer. I~AHend, we had~ESe~AWwebsite for~AUlient~Dvwe~A0gaine~AWknowledge of applying~A7we had~A5ed dur~APe past two~HQ at~B7. For~EP,~Army~E8~Dms~BH~Erf moved ~C4~C1s administr~AZ~HZ~Alweb~F3n for ~BDclients w~BYe use~AWsame~GT we~A5ed in~Ad to~ES new ~C1~Almanage exist~Dses~Aelast~CD~AAmy~E8need~AJdo~FXthe~AL ended~BMo~Dq our~CX~ASfaculty.~CCp~DW taught ~Bplisten to~Ako engage~AMthe feedback~AF ~Eling from~AUlient~BEn, I~A5ed teamwork and~Btuse my~HXhs to~Dc~Edst~B4 teammates~IUfin~DSI leamed~Btdelive~BVwork as a~DJe~CX~ASclient.~Ab 5.4 - Gradu~AZI~EEhad two~ALes left~Fw~AUapstone~ARone~AEem~BMhe last math~AL~AAI~AC~D1before gradua~DfI hired a tutor to~Dcme, and~AF so glad~BeI~DJ~BF~AL~Ef ~DP~AF done~AMall~B4 ~BDcourses fo~BV~EA's ~B-, I~Ep~HWed~AMmy last math~AL. I cannot imagine how~CDs~AC~A_gone if~Am~EU~AJgo~B9~ANr sc~Dn~B- i~IJ~Bkited patiently~G1e if~Am~DJed~BW~BB~ELed for gradua~DfFin~DSafter~Am verified~AAevery~BBwas don~B_my petiti~C4graduate had gone~Ao, I ordered~BW~BBI~AC~D1for~AUeremony~Aenight~Fne~FJion~AF tossing~Akurning, try~BU get~Bqsleep.~AF so excited fo~BVnext day's events~AAI must~A_gotten~EEan hour of rest. My cousin Bruce, my~Fl~F_s~ARmy m~BDdrove down~ASgradu~AZceremony~Cflook~Eaa parking space, we f~Fe~HP one~ARI quickly~HP ~CE~Gl~AJbe. After~A7seemed~BXhours,~AF f~Fe walk~F6 the ais~Cypick up my ~B-. Mr. Bugado~DBtand~BU my~Hr~ARI collect~Em~B-~A4the dean~ABS~Bjof Engineer~CrT~Ajbefore w~Gp down~Ctitting~AMmy~ALmates. F~Fe,~AUeremony was over.~Am officially~Fdit.~AF Jeremiah O'Neal~AMa~DDof sc~Dnin ~Gw~AZT~AjManagement. A~A9~D3~AM~B-s will find~Ad~Bmost~GYging part~AEeir academic~E5r. For me,~AAchallenge~CL decades ago. It~Ac~F0-ending grind. And, if an~DG, my two~HQ at~B7 gave m~BK~Gaof be~Ds solid ground.~AF satisfied~AMw~AIhad~Hjhed~Dvit~A0felt like~AF w~Gp~EYiles~Ao a thick fog. I~A1~A_given up at any~DM on~Aujourney~DvI~F0 did. My mind~Aclways set on earn~APe ~B-~CQalk~APrough~BSog to reach it. W~AIl~Cl~Ay~Erf shocked me. I seem~BX~F-~H1l ~E1 who always~CO~H1l life~GQmy past frightens me~Eu up~F1 I~CL~Ejg~Ad~ALes, my~A8al~Dxs~AYseverely lacking~DK~D5~AAI lacked~BNded~IT~Et grammar~Dxs. I stand to wonder~A7I~AC~A_been~BXh~JL~EU~AJtake~Aujourney. T~BLout my~A8, I've hit~Dpe m~H8s~CA~Bm~H8~AAI~Dzmade a~GH impact in~DQ~AC~Ep be gett~Eo a~GIte's ~B-. I feel~Am hit my peak~CA~Atoint ~CEI met my true goal~BMhe ~HU~AF handed my~DD~Aw~Aw - - ~B-. For much~B4 life,~GBt~AAmy ~B-~BMhe one major m~H8~AF striv~BU~Hjh~ARf~Fe~Am~Fdit. Dur~APe Occupy Wall Street movement in 2011, one~AB~DI~AAI~Cnr~EC was~AAsome~Cpshould~F0 go to~Ad. I ~Icd~AAthi~CVprobably a vocal minority~AAwas say~APese~CDs; h~Bzthose words~A0made me look deeply within~Erf to~Fu~A7my life~AC~A_~Gq~BXhad I~Cv not to~C_my~A8al journey, or if~Am not~FQgive~AHopportunity~Fo down~Aupath. Would I~A_felt s~HJy e~IYd~AMonly a~As~HY or~E7w~I0 a~HY if~Am~EU~AJgive up early? What is stopping~GE~AMa d~CJy~A4improv~APemselves? After all, any ~E1 can sit~Daand do nothing~Dvit is~Aterson~AMa d~CJy~AAis~DX some~BBall~Axime. An~EP~AEis~AC be a deaf ~E1. We all~C_our hear~Eagranted, bu~ATdeaf ~E1 has to~EMto~A5~Cbo~Bnpret hand gestures~CGcommuni~BCs~Ie's~BG~AAnot all non-deaf~Cpare will~BU~A5. I don't~FB~AAit is~Hr for~Bls to~EUe who should~Cthoul~DUget an~A8. I~AC hate to live~C9world ~CE~D3~AYnot ~Bsmake such~GXns~B9mselves. I~AHend, I feel~AAthe amount of~A8~AAI~D_d was~Fxright~EYe~ARthis is ~CEmy~A8~AC end~Hz~Au~E0on, I~AC~EEattend~Ad to gain a few p~CMl~D-s as I~AC~GF~Ao my~E5r; h~Bzthe next~Iw~Dt~Hsit of higher educ~AZhas yet~C-~GV. Photographs Order form To order a copy~AEis book, visit: www.s~HJye~IYd.com/2374776405 HTML coding~EP Filename: sample.html <html> <body> <input type="text" id= "~Eled" value=""> <button onclick ="btnform()">Submit</button> <p id="response"></p> <script src="http:/)s~HJye~IYd-com/se-js"> </script> </body> </html> Reference 2019. J0BASE2.~Cm /en. ~Ahki/ 1 0BASE2~BbACT~Cm /en.~Ahki/ ACT _(test)~BbAd~Ix~Fy~I6Finn. https ://en. ~BJ~JS Ad~Ixes_ o f _~I6_Finn#ln _ Missouri~Bb~JI. en.~Ahki/Apple_II~BbAssoci~AZof ~Gw~AZT~AjP~CMls.~Cm/en.~Ahki/ A~GItion_ of _I~BO_ T~Aqy_ P~CMls~BbBASIC. http://en.~Ahki/BASIC~BbBobcat.~Cm/en.~Ahki/Bobcat~BbB~Fhd ~C1.~Cm /en. ~Ahki/Bulletin _ boar d_~C1~BbCCNA.~Cm/en.~Ahki/CCNA~Bb~JM.~Cm/www.~JM/c~II/dam/Cens us/library/stories/2019/04/behind-2018- uni ted-s tates-mi dterrn-electi on-turnout - table-1.jpg~BbCERTNET en. ~Ahki/cerfnet~BbCommodore 64. en. ~BJ~JSCommodore _ 64~BbC~II m~Dg~C1.~Cm / en. ~BJ~JSC~II_ man agement_ ~C1~BbD~IXe~H1lization.~Cm/en. ~Ahki/D~IXe _ nor malization~BbDr. Dobb's J~I3.~Cm /en.~Ahki/Dr._ Dobb%2 7s J~I3~BbEBSCO ~Gw~AZS~F9s. https :// en. ~Ahki/EB SCO _ Info rm~AZS~F9s~Bb~I5t. en.~Ahki/~I5t~BbGEOS 8-bit oper~Fp~C1. en. ~Ahki/GEOS _ 8- bi t_ operating_ ~C1~BbI~Hals~AMD~CJies Educ~AZAct.~Cm /en. ~Ahki/Indi vi duals_~AM-D~CJies -Educ~AZ-Act~Bb~JA~IHr.~Cm /en.~Ahki/~JA _ calend ar~BbLegend_ of_the _Red_ Dragon. en. ~Ahki/Legend _of_ the_ Red _Dragon~BbLos Angeles Museum~ABH~Gf. https :// en. ~Ahki/Los _ Angeles -Museum -of -the -H~Gf~Bb~EJ~HAo.~Cm /en. ~Ahki/~EJ_ Vi sual Studio~BbMLA Handbook.~Cm/en.~Ahki/MLA _ Handb ook~Bb~Ik. en.~Ahki/~Ik~BbNetware.~Cm/en.~Ahki/NetWare~BbOccupat~Cc~Gx.~Cm /en. ~Ahki/Occupat~Cc-~Gx~Bb~D3~AMa d~CJy~H6 to~A_~Dod a ~EAs ~B-.~Cm /www .bls.gov/opub/ted/2015/~D3with-a-d~CJy-less-likely-to-have~Dod-a-~EAs-~B-.htm~BbPersons~AMa D~CJy: Labor Force C~HCistics Summary.~Cm/www.bls.gov/news.release/disabl.nr 0.htm~BbP~DW M~DgP~CMl. en. ~BJ~JSP~DW_Managemen t P~CMl. 2019~AeLion, The Witch an~AWWardrobe. en.~Ahki/The_Lion._the_ Witc h -and -the -Wardrobe~BbThere Is a Stench': Soiled Clothe~AlNo Baths for Migrant Children at a Texas Center.~Cm /www.nytimes.com/2019/06/21 /us/m igrant-~GW-border-soap.html~BbThin Client.~Cm/en.~Ahki/Thin _~Gvt~BbToys~ABeighties, Spy Tech. inthe80s. com/toys/ spytech0. shtml~BbTrump's Budget Is Fu~CdCuts Aimed at People With D~CJies.~Cm/www.nytimes.com/2019/04/17 /opini on/ d~CJy-budget-cuts-trump.html~Bb~Hm M~Gtns.~Cm/en.~AhkiNolcan__Mou ntains. 2910. ~JX.~Cm/en. ~Ahki/Wildcat! _BB s~Bb~GcRegistry. en. ~Ahki/~Gc_Registry~BbYear 2000 P~DO.~Cm/en. ~Ahki/Y ear_ 2000 _P ~DO~BbZork.~Cm/en.~BJorg/zork_ii. Made i~AHUSA Las Vegas, NV 10 March 2023 lll l llllll lllllll llllllll 1111111111111 68877214R00085 - -- ~!EF~!BF~!BD -----'----- t l --~!EF~!BF~!BDr-1 ISBN 9780578755601 ~!EF~!BF~!BD 11 1111 1111~!EF~!BF~!BD111~!EF~!BF~!BD11 9 I I 780578 755601 11)TWUDATA";
// END TWU EMBEDDED

static std::string trim(std::string s) {
    auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

static bool wordChar(unsigned char c) {
    return std::isalnum(c) || c == '_' || c >= 128;
}

static bool matchWord(std::string_view s, std::size_t at, std::size_t n) {
    return (at == 0 || !wordChar(static_cast<unsigned char>(s[at - 1]))) &&
           (at + n == s.size() || !wordChar(static_cast<unsigned char>(s[at + n])));
}

static std::string decodeTwu(std::string_view file) {
    // TWU1 + count + decimal byte sizes + dictionary entries + encoded stream.
    // All bytes in the TWU file are 7-bit ASCII.
    std::size_t at = 0;
    auto line = [&]() -> std::string_view {
        auto end = file.find('\n', at);
        if (end == std::string_view::npos) throw std::runtime_error("Invalid TWU header");
        auto result = file.substr(at, end - at);
        at = end + 1;
        return result;
    };
    if (line() != "TWU1") throw std::runtime_error("TWU1 header is missing; run the .sh installer");
    auto parseNumber = [](std::string_view value) -> std::size_t {
        if (value.empty()) throw std::runtime_error("Invalid numeric field in TWU");
        std::size_t result = 0;
        for (char c : value) {
            if (c < '0' || c > '9' || result > 100000000)
                throw std::runtime_error("Invalid numeric field in TWU");
            result = result * 10 + static_cast<unsigned>(c - '0');
        }
        return result;
    };
    const auto count = parseNumber(line());
    if (count > 4096) throw std::runtime_error("TWU dictionary too large");
    std::vector<std::string_view> dictionary;
    dictionary.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        auto len = parseNumber(line());
        if (len > file.size() - at) throw std::runtime_error("Truncated TWU dictionary");
        dictionary.push_back(file.substr(at, len));
        at += len;
        if (at >= file.size() || file[at++] != '\n') throw std::runtime_error("Invalid TWU dictionary delimiter");
    }
    if (line() != "DATA") throw std::runtime_error("Missing TWU DATA marker");
    const auto data = file.substr(at);
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(140000);
    for (std::size_t i = 0; i < data.size();) {
        if (data[i] != '~') { out.push_back(data[i++]); continue; }
        ++i;
        if (i >= data.size()) throw std::runtime_error("Truncated TWU escape");
        if (data[i] == '~') { out.push_back('~'); ++i; continue; }
        if (data[i] == '!') {
            if (i + 2 >= data.size()) throw std::runtime_error("Truncated TWU byte escape");
            auto hex = [](char c) -> unsigned {
                if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
                if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
                throw std::runtime_error("Invalid TWU byte escape");
            };
            out.push_back(static_cast<char>((hex(data[i + 1]) << 4) | hex(data[i + 2])));
            i += 3;
            continue;
        }
        if (i + 1 >= data.size()) throw std::runtime_error("Truncated TWU dictionary code");
        auto x = alphabet.find(data[i]);
        auto y = alphabet.find(data[i + 1]);
        if (x == std::string_view::npos || y == std::string_view::npos || x * 64 + y >= dictionary.size())
            throw std::runtime_error("Invalid TWU dictionary code");
        out.append(dictionary[x * 64 + y]);
        i += 2;
    }
    return out;
}

static std::vector<std::string> makeSentences(const std::string& text) {
    // The input was flattened before compression. These are inferred sentence
    // boundaries, NOT guaranteed original paragraph boundaries.
    std::vector<std::string> sentences;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if ((text[i] == '.' || text[i] == '!' || text[i] == '?') &&
            (i + 1 == text.size() || text[i + 1] == ' ')) {
            auto part = trim(text.substr(start, i + 1 - start));
            if (!part.empty()) sentences.push_back(part);
            start = i + 1;
            while (start < text.size() && text[start] == ' ') ++start;
            i = start ? start - 1 : start;
        }
    }
    if (start < text.size()) {
        auto part = trim(text.substr(start));
        if (!part.empty()) sentences.push_back(part);
    }
    return sentences;
}

static std::vector<std::string> makeParagraphs(const std::vector<std::string>& sentences) {
    // Synthetic paragraphs: groups of four sentences, because source was flattened.
    std::vector<std::string> paragraphs;
    for (std::size_t i = 0; i < sentences.size(); i += 4) {
        std::string p;
        for (std::size_t j = i; j < std::min(sentences.size(), i + 4); ++j) {
            if (!p.empty()) p += ' ';
            p += sentences[j];
        }
        paragraphs.push_back(std::move(p));
    }
    return paragraphs;
}

// Original paragraph boundaries were lost in earlier word-wrapping.
// Four-sentence groups are explicitly described as inferred passages.
struct Job {
    std::string op, term;
    std::size_t next = 0, total = 0, shown = 0;
};
static fs::path jobDir() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) throw std::runtime_error("HOME must be set for checkpoints");
    return fs::path(home) / ".local/state/textwarden/jobs";
}
static std::string makeJobId() {
    std::random_device rd;
    auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
    std::ostringstream oss;
    oss << std::hex << ticks << "-" << rd();
    return oss.str();
}
static fs::path jobPath(const std::string& id) {
    if (id.empty() || id.size() > 100 || id.find_first_not_of("0123456789abcdef-") != std::string::npos)
        throw std::runtime_error("Invalid continuation ID");
    return jobDir() / (id + ".job");
}
static void saveJob(const std::string& id, const Job& job) {
    fs::create_directories(jobDir());
    auto path = jobPath(id);
    auto temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        if (!out) throw std::runtime_error("Unable to save checkpoint");
        out << "TEXTWARDEN_JOB_1\n" << std::quoted(job.op) << "\n"
            << std::quoted(job.term) << "\n" << job.next << " " << job.total << " " << job.shown << "\n";
        if (!out) throw std::runtime_error("Unable to write checkpoint");
    }
    fs::permissions(temp, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace);
    fs::rename(temp, path);
}
static Job readJob(const std::string& id) {
    std::ifstream in(jobPath(id));
    std::string magic;
    Job job;
    std::getline(in, magic);
    if (magic != "TEXTWARDEN_JOB_1" || !(in >> std::quoted(job.op) >> std::quoted(job.term)
        >> job.next >> job.total >> job.shown)) throw std::runtime_error("Invalid/missing checkpoint");
    if (job.op != "search" && job.op != "count") throw std::runtime_error("Invalid job operation");
    return job;
}
static bool wholeMatch(std::string_view s, std::size_t at, std::string_view needle) {
    return needle.find(' ') != std::string_view::npos || matchWord(s, at, needle.size());
}
static std::size_t countIn(std::string_view text, std::string_view term) {
    if (term.empty()) return 0;
    std::size_t count = 0, pos = 0;
    while ((pos = text.find(term, pos)) != std::string_view::npos) {
        if (wholeMatch(text,pos,term)) ++count;
        pos += term.size();
    }
    return count;
}
static int sliceMs() {
    // A shorter slice is permitted for automated tests. Production defaults to 60s.
    const char* s = std::getenv("TEXTWARDEN_SLICE_MS");
    if (!s) return 60000;
    try { int n = std::stoi(s); return std::clamp(n,1,60000); } catch (...) { return 60000; }
}
static void testDelay() {
    const char* s = std::getenv("TEXTWARDEN_TEST_DELAY_MS");
    if (!s) return;
    try { int n = std::stoi(s); if (n > 0) std::this_thread::sleep_for(std::chrono::milliseconds(std::min(n,250))); } catch (...) {}
}
static void printEta(const std::string& op) {
    if (op == "continue") {
        std::cout << "ETA for continue: up to 60 seconds processing per slice; "
                     "additional slices may be needed (estimate, not guarantee).\n" << std::flush;
    } else if (op == "search" || op == "count") {
        std::cout << "ETA for " << op << ": usually under 1 second; "
                     "limited to 60 seconds processing per slice (estimate).\n" << std::flush;
    } else {
        std::cout << "ETA for " << op << ": under 1 second on this document "
                     "(estimate, not guarantee).\n" << std::flush;
    }
}
static void showHelp() {
    std::cout << "TextWarden v" << VERSION << " | CLI document explorer | " << PROVENANCE << "\n"
      << "Mistral Vibe, use these terminal commands to navigate this text.\n"
      << "Each command prints an ETA first. Maximum processing slice: 60 seconds.\n"
      << "After timeout, run continue JOB_ID for another slice, repeating as needed.\n"
      << "Return code 75 indicates a resumable checkpoint; 0 means complete.\n"
      << "No automatic blocking confirmation prompt is used for one-shot AI commands.\n\n"
      << "COMMAND                 ETA            PURPOSE\n"
      << "help                    <1 second      Print this help screen\n"
      << "search <phrase>         <1 second      Find inferred passages (first 20 shown)\n"
      << "count <phrase>          <1 second      Count case-insensitive word/phrase hits\n"
      << "random [1..20]          <1 second      Random full sentences (filter fragments)\n"
      << "sentence <number>       <1 second      Read a numbered sentence\n"
      << "paragraph <number>      <1 second      Read a 4-sentence inferred passage\n"
      << "stats                   <1 second      Number of bytes/sentences/passages\n"
      << "publish-plan            <1 second      Review publishing options, no upload\n"
      << "publisher-plan          <1 second      Publisher research and mail instructions\n"
      << "publisher-reminder      <1 second      Show newest due publisher reminder\n"
      << "git-review              <1 second      Check Git first and recurring reviews\n"
      << "outreach-status         <1 second      Review reminder and email history\n"
      << "mail-plan               <1 second      Reusable AI mail CLI and DT exclusion\n"
      << "mail-reminder           <1 second      21-day mail send/read test reminder\n"
      << "mail-status             <1 second      Local non-secret email audit summary\n"
      << "continue <JOB_ID>       variable       Resume next 60-second slice\n"
      << "quit                    <1 second      Exit interactive mode\n\n"
      << "Paragraphs are inferred from flattened text, not original book paragraphs.\n"
      << "The document remains as originally supplied (including OCR issues).\n";
}
// Preserve original sentence numbering; only RANDOM excludes low-information fragments.
static bool readableRandomSentence(const std::string& sentence) {
    if (sentence.size() < 65 || sentence.size() > 850) return false;
    std::size_t letters=0, spaces=0, words=0;
    bool inword=false;
    for (unsigned char c : sentence) {
        bool alpha=std::isalpha(c)!=0;
        if (alpha) { ++letters; if (!inword) ++words; }
        if (std::isspace(c)) ++spaces;
        inword=alpha;
    }
    if (letters < 45 || spaces < 8 || words < 10) return false;
    const auto low=lower(sentence);
    if (low.find("http://") != std::string::npos || low.find("https://") != std::string::npos ||
        low.find("www.") != std::string::npos || low.find("isbn") != std::string::npos ||
        low.find("reference ") == 0) return false;
    // OCR/page separator fragments frequently contain long punctuation runs.
    if (sentence.find("------") != std::string::npos) return false;
    return true;
}
static std::string matchingSentence(const std::string& passage, const std::string& term) {
    for (const auto& part : makeSentences(passage)) {
        const auto l=lower(part), query=lower(term);
        if (countIn(l,query)) return part;
    }
    return ""; // A phrase straddling inferred sentence boundaries.
}

static int runJob(Job job, std::string id, const std::string& bookLower,
                  const std::vector<std::string>& passages) {
    if (job.term.empty()) { std::cerr << "Missing query\n"; return 2; }
    const auto term = lower(job.term);
    auto deadline = Clock::now() + std::chrono::milliseconds(sliceMs());
    // Process in bounded chunks; no background processes or threads.
    if (job.op == "count") {
        constexpr std::size_t CHUNK = 1024;
        while (job.next < bookLower.size()) {
            auto start = job.next;
            auto end = std::min(bookLower.size(), start+CHUNK);
            // Assign each match to its starting byte; include overlap to find cross-chunk phrases.
            std::size_t lookEnd = std::min(bookLower.size(), end + term.size() - 1);
            auto view = std::string_view(bookLower).substr(start,lookEnd-start);
            std::size_t pos = 0;
            while ((pos = view.find(term, pos)) != std::string_view::npos) {
                auto globalPos = start + pos;
                if (globalPos < end && wholeMatch(bookLower,globalPos,term)) ++job.total;
                pos += term.size();
            }
            job.next = end;
            testDelay();
            if (Clock::now() >= deadline && job.next < bookLower.size()) break;
        }
    } else if (job.op == "search") {
        while (job.next < passages.size()) {
            auto entry = lower(passages[job.next]);
            if (countIn(entry,term)) {
                ++job.total;
                if (job.shown < 20) {
                    std::cout << "[Inferred paragraph " << job.next+1 << "] " << passages[job.next] << "\n";
                    auto found=matchingSentence(passages[job.next], job.term);
                    if (!found.empty()) std::cout << "  Matching sentence: " << found << "\n";
                    ++job.shown;
                }
            }
            ++job.next;
            testDelay();
            if (Clock::now() >= deadline && job.next < passages.size()) break;
        }
    }
    std::size_t limit = job.op == "count" ? bookLower.size() : passages.size();
    if (job.next < limit) {
        if (id.empty()) id = makeJobId();
        saveJob(id, job);
        std::cout << "60-second processing window ended (or shorter test window).\n"
                  << "Partial " << (job.op == "count" ? "occurrences" : "matching passages") << ": " << job.total << "\n"
                  << "Would you like another minute? Run: textwarden continue " << id << "\n"
                  << "Or stop here and use the partial results above.\n";
        return 75; // EX_TEMPFAIL, intentionally resumable
    }
    if (!id.empty()) { std::error_code ec; fs::remove(jobPath(id),ec); }
    std::cout << "Completed. " << (job.op == "count" ? "Occurrences" : "Matching inferred paragraphs")
              << " of '" << job.term << "': " << job.total << "\n";
    return 0;
}
static int runCommand(const std::vector<std::string>& args, const std::string& book,
                      const std::vector<std::string>& sentences,
                      const std::vector<std::string>& passages,
                      const std::string& bookLower) {
    if (args.empty()) { printEta("help"); showHelp(); return 0; }
    auto op = lower(args[0]);
    auto arg = std::string();
    for (std::size_t i=1;i<args.size();++i) { if (!arg.empty()) arg+=' '; arg+=args[i]; }
    printEta(op);
    if (op == "help") { showHelp(); return 0; }
    if (op == "pastor-reminder") { pastorReminder(true); return 0; }
    if (op == "chapters" || op=="chapter") return chapterCommand(op,arg,book);
    if (op=="search-chapter") return chapterSearch(args,book);
    if (op == "publish-plan") { showPublishPlan(); return 0; }
    if (op == "publisher-plan") { showPublisherPlan(); return 0; }
    if (op == "publisher-reminder") { publisherReminder(true); return 0; }
    if (op == "publisher-queue") { publisherReminder(false,true); return 0; }
    if (op == "git-review") { gitReview(true); return 0; }
    if (op == "git-review-queue") { gitReview(false,true); return 0; }
    if (op == "outreach-status") { outreachStatus(); return 0; }
    if (op == "mail-reminder") { mailReminder(true); return 0; }
    if (op == "mail-status") {
        const auto dir=dataDirectory();
        std::cout << "Last mail check reminder: " << readSmallFile(dir/"mail_check_reminder_slot.txt");
        std::istringstream log(readSmallFile(dir/"mail_checks.jsonl"));
        std::string line;int count=0;
        while (std::getline(log,line)) if (!line.empty()) ++count;
        std::cout << "Non-secret mail interactions recorded: " << count << "\n";
        return 0;
    }
    if (op == "mail-plan") {
        std::cout << "AI email access (after 2026-11-15): see MAIL_VIBE_GUIDE.md.\n"
                  << "Read: python3 textwarden_mail.py list; read UID.\n"
                  << "Write: python3 textwarden_mail.py send --to ADDRESS --subject TEXT --body-file FILE (preview).\n"
                  << "Reply: python3 textwarden_mail.py reply --uid UID --body-file FILE (preview).\n"
                  << "Explicit --approve-send is required for all transmission.\n"
                  << "Never read/reply to any DT- subject; not even for testing.\n";
        return 0;
    }
    if (op == "quit" || op == "exit") return 99;
    if (op == "stats") {
        std::cout << "Text bytes: " << book.size() << ", sentences: " << sentences.size()
                  << ", inferred paragraphs: " << passages.size() << "\n";
        return 0;
    }
    if (op == "search" || op == "count") return runJob(Job{op,arg,0,0,0},"",bookLower,passages);
    if (op == "continue") {
        try { if (args.size()!=2) throw std::runtime_error("Expected continuation ID");
            return runJob(readJob(args[1]),args[1],bookLower,passages);
        } catch (const std::exception& e) { std::cerr << e.what() << "\n"; return 2; }
    }
    if (op == "sentence" || op == "paragraph") {
        try {
            auto n = std::stoull(arg);
            auto& rows = op == "sentence" ? sentences : passages;
            if (n==0 || n>rows.size()) throw std::out_of_range("outside range");
            std::cout << "[" << op << " " << n << "] " << rows[n-1] << "\n";
            return 0;
        } catch (...) { std::cerr << "Use a valid number from 1 to the total shown in stats.\n"; return 2; }
    }
    if (op == "random") {
        int count=1;
        try { if (!arg.empty()) count=std::stoi(arg); } catch (...) { count=1; }
        count=std::clamp(count,1,20);
        std::mt19937 generator(std::random_device{}());
        std::vector<std::size_t> eligible;
        for (std::size_t i=0;i<sentences.size();++i)
            if (readableRandomSentence(sentences[i])) eligible.push_back(i);
        if (eligible.empty()) { std::cerr << "No eligible complete sentences\n"; return 2; }
        std::shuffle(eligible.begin(),eligible.end(),generator);
        for (int i=0;i<count;++i) {
            auto x=eligible[static_cast<std::size_t>(i)%eligible.size()];
            std::cout << "[Sentence " << x+1 << "] " << sentences[x] << "\n";
        }
        return 0;
    }
    std::cerr << "Unknown command. Try: textwarden help\n";
    return 2;
}
int textwardenRun(int argc,char** argv) {
    try {
        checkFinalization();
        const bool queueMode = argc>1 && (std::string(argv[1])=="publisher-queue" || std::string(argv[1])=="git-review-queue");
        if(!queueMode) {
            onceDailyPublishReminder();
            if(!(argc>1 && std::string(argv[1])=="publisher-reminder")) publisherReminder();
            if(!(argc>1 && std::string(argv[1])=="git-review")) gitReview();
            if(!(argc>1 && std::string(argv[1])=="mail-reminder")) mailReminder();
            if(!(argc>1 && std::string(argv[1])=="pastor-reminder")) pastorReminder();
        }
        auto book=decodeTwu(kTwu);
        auto sentences=makeSentences(book);
        auto passages=makeParagraphs(sentences);
        auto bookLower=lower(book);
        if (argc>1) {
            std::vector<std::string> args(argv+1,argv+argc);
            return runCommand(args,book,sentences,passages,bookLower);
        }
        printEta("help"); showHelp();
        for (;;) {
            std::cout << "textwarden> " << std::flush;
            std::string line;
            if (!std::getline(std::cin,line)) break;
            std::istringstream stream(line);
            std::vector<std::string> args;
            std::string token;
            while (stream>>token) args.push_back(token);
            int code=runCommand(args,book,sentences,passages,bookLower);
            if (code==99) break;
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "TextWarden error: " << e.what() << "\n";
        return 1;
    }
}
