#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>
using namespace std;

struct Config {
    int frames = 64;          // N фізичних сторінок
    int vpages = 100;         // віртуальних сторінок у процесі
    int ws_size = 12;         // розмір робочого набору
    int max_procs = 4;        // максимум одночасних процесів
    long total = 200000;      // загальна кількість звернень (тривалість моделі)
    int quantum = 50;         // квант часу
    int life = 20000;         // середня тривалість життя процесу
    int spawn_every = 3000;   // інтервал створення нових процесів
    int ws_change = 1000;     // як часто змінюється робочий набір процесу
    int timer = 200;          // період таймера (скидання біта R)
    int tau = 400;            // поріг віку для WSClock
    int max_writes = 4;       // макс. записів у ФС за один сторінковий промах (WSClock)
    double ws_prob = 0.9;     // частка звернень до робочого набору
    double write_prob = 0.3;  // частка модифікацій серед звернень
    unsigned seed = 1;
    bool verbose = false;
    string alg = "clock";
};

struct Stats {
    long accesses = 0, faults = 0, fs_reads = 0, fs_writes = 0;
    long zero_fills = 0, evictions = 0, created = 0, finished = 0;
};
struct PTE {
    bool P = false, R = false, M = false;  // присутність, звернення, модифікація
    int ppn = -1;                          // номер фізичної сторінки
    bool in_fs = false;  // облік ядра: вміст уже записано
};

struct PageTable {
    vector<PTE> e;
    explicit PageTable(int n) : e(n) {}
};

struct PhysicalPage {
    bool free = true;
    int pid = -1, vpn = -1;
    long last_use = 0;  // для WSClock
};

struct WorkingSet {
    vector<int> pages;
    void generate(mt19937& r, int vpages, int size) {
        pages.clear();
        size = min(size, vpages);
        while ((int)pages.size() < size) {
            int v = r() % vpages;
            if (find(pages.begin(), pages.end(), v) == pages.end()) pages.push_back(v);
        }
    }
    void shift(mt19937& r, int vpages) {  // змінюємо половину робочого набору
        int k = max(1, (int)pages.size() / 2);
        for (int i = 0; i < k; i++) {
            int v, idx = r() % pages.size();
            do { v = r() % vpages; } while (find(pages.begin(), pages.end(), v) != pages.end()
                                            && (int)pages.size() < vpages);
            pages[idx] = v;
        }
    }
};

class Kernel;

struct Process {
    int pid;
    PageTable pt;
    WorkingSet ws;
    long executed = 0, life;
    Process(int pid, int vpages, long life) : pid(pid), pt(vpages), life(life) {}
    bool done() const { return executed >= life; }
    // повертає (номер віртуальної сторінки, чи це модифікація)
    pair<int, bool> next_access(Kernel& k);
};

struct ReplacementPolicy {
    virtual ~ReplacementPolicy() {}
    virtual const char* name() = 0;
    virtual int pick(Kernel& k) = 0;     // вибір фізичної сторінки на заміну
    virtual void tick(Kernel&) {}        // дія по таймеру
};

class Kernel {
public:
    Config cfg;
    mt19937 rng;
    long now = 0;
    vector<PhysicalPage> frames;
    unordered_map<int, Process*> procs;
    Stats st;
    unique_ptr<ReplacementPolicy> policy;
    int next_pid = 1;

    explicit Kernel(const Config& c);

    PTE& pte(int ppn) {
        PhysicalPage& f = frames[ppn];
        return procs[f.pid]->pt.e[f.vpn];
    }
    void log(const char* fmt, ...) {
        if (!cfg.verbose) return;
        printf("[t=%ld] ", now);
        va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a);
        printf("\n");
    }
    int find_free() {
        for (int i = 0; i < (int)frames.size(); i++) if (frames[i].free) return i;
        return -1;
    }
    // записати вміст брудної сторінки у ФС, сторінка лишається відображеною
    void flush(int ppn) {
        PTE& e = pte(ppn);
        st.fs_writes++;
        e.M = false;
        e.in_fs = true;
        log("запис у ФС: pid=%d vpn=%d (ppn=%d)", frames[ppn].pid, frames[ppn].vpn, ppn);
    }
    void evict(int ppn) {
        PhysicalPage& f = frames[ppn];
        PTE& e = pte(ppn);
        log("заміна: ppn=%d (pid=%d vpn=%d R=%d M=%d)", ppn, f.pid, f.vpn, e.R, e.M);
        if (e.M) flush(ppn);
        e.P = e.R = e.M = false;
        e.ppn = -1;
        f.free = true;
        st.evictions++;
    }
    // обробка сторінкового промаху
    void handle_fault(Process& p, int vpn) {
        st.faults++;
        PTE& e = p.pt.e[vpn];
        log("СТОРІНКОВИЙ ПРОМАХ: pid=%d vpn=%d", p.pid, vpn);
        int ppn = find_free();
        if (ppn < 0) {
            ppn = policy->pick(*this);
            evict(ppn);
        }
        frames[ppn] = PhysicalPage{false, p.pid, vpn, now};
        if (e.in_fs) { st.fs_reads++; log("читання з ФС"); }
        else { st.zero_fills++; log("перше звернення, нульова сторінка"); }
        e.P = true; e.R = false; e.M = false; e.ppn = ppn;
    }
    void timer_tick() { policy->tick(*this); }

    Process* create_process() {
        long life = cfg.life / 2 + rng() % cfg.life;
        Process* p = new Process(next_pid++, cfg.vpages, life);
        p->ws.generate(rng, cfg.vpages, cfg.ws_size);
        procs[p->pid] = p;
        st.created++;
        log("створено процес pid=%d, тривалість=%ld", p->pid, life);
        return p;
    }
    void destroy_process(Process* p) {
        for (auto& f : frames) if (!f.free && f.pid == p->pid) f.free = true;
        procs.erase(p->pid);
        st.finished++;
        log("завершено процес pid=%d", p->pid);
        delete p;
    }
};

struct MMU {
    // трансляція: при відсутності сторінки - виняток (page fault) до ядра
    static void access(Kernel& k, Process& p, int vpn, bool write) {
        PTE* e = &p.pt.e[vpn];
        if (!e->P) {
            k.handle_fault(p, vpn);
            e = &p.pt.e[vpn];
        }
        e->R = true;
        if (write) e->M = true;
        k.frames[e->ppn].last_use = k.now;  // лише для статистики WSClock (див. tick)
    }
};

pair<int, bool> Process::next_access(Kernel& k) {
    uniform_real_distribution<double> u(0, 1);
    int vpn;
    if (u(k.rng) < k.cfg.ws_prob) vpn = ws.pages[k.rng() % ws.pages.size()];
    else vpn = k.rng() % k.cfg.vpages;
    return {vpn, u(k.rng) < k.cfg.write_prob};
}
struct RandomPolicy : ReplacementPolicy {
    const char* name() override { return "Random"; }
    int pick(Kernel& k) override { return k.rng() % k.frames.size(); }
};
struct NRUPolicy : ReplacementPolicy {
    const char* name() override { return "NRU"; }
    int pick(Kernel& k) override {
        vector<int> cls[4];
        for (int i = 0; i < (int)k.frames.size(); i++) {
            PTE& e = k.pte(i);
            cls[2 * e.R + e.M].push_back(i);
        }
        for (int c = 0; c < 4; c++)
            if (!cls[c].empty()) {
                k.log("NRU: клас %d, кандидатів %zu", c, cls[c].size());
                return cls[c][k.rng() % cls[c].size()];
            }
        return 0;
    }
    void tick(Kernel& k) override {  // фоновий скид біта R; M не чіпаємо
        for (auto& f : k.frames)
            if (!f.free) k.procs[f.pid]->pt.e[f.vpn].R = false;
    }
};

struct ClockPolicy : ReplacementPolicy {
    int hand = 0;
    const char* name() override { return "Clock"; }
    int pick(Kernel& k) override {
        int n = k.frames.size();
        for (;;) {
            int f = hand;
            hand = (hand + 1) % n;
            PTE& e = k.pte(f);
            if (!e.R) return f;
            e.R = false;
        }
    }
    void tick(Kernel& k) override {  // фоновий скид біта R (щоб R відображав недавні звернення)
        for (auto& f : k.frames)
            if (!f.free) k.procs[f.pid]->pt.e[f.vpn].R = false;
    }
};

struct WSClockPolicy : ReplacementPolicy {
    int hand = 0;
    const char* name() override { return "WSClock"; }
    void tick(Kernel& k) override {
        // фонове оновлення статистики: якщо було звернення - запам'ятати час і скинути R
        for (auto& f : k.frames)
            if (!f.free) {
                PTE& e = k.procs[f.pid]->pt.e[f.vpn];
                if (e.R) { f.last_use = k.now; e.R = false; }
            }
    }
    int pick(Kernel& k) override {
        int n = k.frames.size(), writes = 0, clean_candidate = -1;
        for (int step = 0; step < 2 * n; step++) {
            int f = hand;
            hand = (hand + 1) % n;
            PTE& e = k.pte(f);
            if (e.R) { e.R = false; k.frames[f].last_use = k.now; continue; }
            long age = k.now - k.frames[f].last_use;
            if (age > k.cfg.tau) {
                if (!e.M) return f;                 // стара й чиста - замінюємо
                if (writes < k.cfg.max_writes) {    // стара брудна - записуємо, шукаємо далі
                    k.flush(f);
                    writes++;
                }
            } else if (!e.M && clean_candidate < 0) {
                clean_candidate = f;                // запасний варіант
            }
        }
        if (clean_candidate >= 0) return clean_candidate;
        return hand;  // "поточна" сторінка
    }
};

Kernel::Kernel(const Config& c) : cfg(c), rng(c.seed), frames(c.frames) {
    if (c.alg == "random") policy.reset(new RandomPolicy);
    else if (c.alg == "nru") policy.reset(new NRUPolicy);
    else if (c.alg == "wsclock") policy.reset(new WSClockPolicy);
    else policy.reset(new ClockPolicy);
}

Stats run(const Config& cfg) {
    Kernel k(cfg);
    vector<Process*> alive;
    size_t rr = 0;
    long next_spawn = 0;
    while (k.now < cfg.total) {
        if (alive.empty() || (k.now >= next_spawn && (int)alive.size() < cfg.max_procs)) {
            alive.push_back(k.create_process());
            next_spawn = k.now + cfg.spawn_every;
        }
        if (rr >= alive.size()) rr = 0;
        Process* p = alive[rr];
        k.log("--- процесорний час отримує pid=%d", p->pid);
        for (int q = 0; q < cfg.quantum && k.now < cfg.total && !p->done(); q++) {
            k.now++;
            k.st.accesses++;
            if (p->executed % cfg.ws_change == 0 && p->executed > 0) {
                p->ws.shift(k.rng, cfg.vpages);
                k.log("pid=%d: робочий набір змінено", p->pid);
            }
            auto a = p->next_access(k);
            k.log("pid=%d %s vpn=%d", p->pid, a.second ? "WRITE" : "READ ", a.first);
            MMU::access(k, *p, a.first, a.second);
            p->executed++;
            if (k.now % cfg.timer == 0) k.timer_tick();
        }
        if (p->done()) {
            alive.erase(alive.begin() + rr);
            k.destroy_process(p);
        } else rr++;
    }
    Stats s = k.st;
    for (Process* p : alive) k.destroy_process(p);
    if (cfg.verbose || true) {
        // звіт про характеристики віртуальної пам'яті виводиться з main
    }
    return s;
}

static void print_stats(const Config& c, const Stats& s) {
    printf("Алгоритм: %-8s  N=%d  vpages=%d  ws=%d\n", c.alg.c_str(), c.frames, c.vpages, c.ws_size);
    printf("  звернень: %ld, процесів створено/завершено: %ld/%ld\n", s.accesses, s.created, s.finished);
    printf("  сторінкових промахів: %ld (%.2f%%)\n", s.faults, 100.0 * s.faults / s.accesses);
    printf("  нульових сторінок: %ld, читань з ФС: %ld, записів у ФС: %ld, замін: %ld\n",
           s.zero_fills, s.fs_reads, s.fs_writes, s.evictions);
}

int main(int argc, char** argv) {
    Config c;
    bool sweep = false;
    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        auto val = [&]() { return i + 1 < argc ? argv[++i] : (char*)"0"; };
        if (a == "--alg") c.alg = val();
        else if (a == "--frames") c.frames = atoi(val());
        else if (a == "--vpages") c.vpages = atoi(val());
        else if (a == "--ws") c.ws_size = atoi(val());
        else if (a == "--procs") c.max_procs = atoi(val());
        else if (a == "--steps") c.total = atol(val());
        else if (a == "--seed") c.seed = atoi(val());
        else if (a == "--tau") c.tau = atoi(val());
        else if (a == "--verbose") c.verbose = true;
        else if (a == "--sweep") sweep = true;
        else {
            printf("Використання: %s [--alg random|nru|clock|wsclock] [--frames N] [--vpages V] [--ws S]\n"
                   "  [--procs M] [--steps T] [--seed X] [--tau T] [--verbose] [--sweep]\n", argv[0]);
            return 1;
        }
    }
    if (sweep) {  // перевірка моделі: частота промахів зростає зі збільшенням робочого набору
        const char* algs[] = {"random", "nru", "clock", "wsclock"};
        printf("N=%d, процесів до %d. Частота сторінкових промахів, %%\n", c.frames, c.max_procs);
        printf("%6s", "ws");
        for (auto a : algs) printf("%10s", a);
        printf("\n");
        for (int ws : {4, 8, 12, 16, 20, 24, 32, 40}) {
            printf("%6d", ws);
            for (auto a : algs) {
                Config d = c; d.alg = a; d.ws_size = ws; d.verbose = false;
                Stats s = run(d);
                printf("%10.2f", 100.0 * s.faults / s.accesses);
            }
            printf("\n");
        }
        return 0;
    }
    Stats s = run(c);
    print_stats(c, s);
    return 0;
}
