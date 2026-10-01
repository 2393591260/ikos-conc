// BMC race-encoding prototype: verify the "adjacent <=> race" trick from
// Dartagnan's SC encoding (svcomp.cat: hb = po | com | sync, race = conflict \ hb).
//
// Encodes the events of 04-mutex_01-simple_rc.c as integer clocks + MUST
// happens-before edges (program order + mutex unlock->lock), then asks Z3
// whether the two racing accesses can be ADJACENT in the clock order.
//
//   race case (mutex1 vs mutex2):  SAT  (adjacent possible -> real race)
//   safe case (same mutex):         UNSAT (sync path forces them apart)
//
// Build:  g++ -std=c++17 bmc_race_encoding.cpp -lz3 -o bmc_race_encoding

#include <z3++.h>
#include <iostream>
#include <string>
#include <vector>

struct Event {
  std::string name;
  int thread;  // 0 = t_fun, 1 = main
};

int main() {
  // ---- Events (global ids) ----
  // t_fun:  L1 lock mutex1 -> R1 load myglobal -> W1 store myglobal -> U1 unlock mutex1
  // main:   L2 lock mutex2 -> R2 load myglobal -> W2 store myglobal -> U2 unlock mutex2 -> J join
  std::vector<Event> ev = {
      {"L1", 0}, {"R1", 0}, {"W1", 0}, {"U1", 0},
      {"L2", 1}, {"R2", 1}, {"W2", 1}, {"U2", 1}, {"J", 1},
  };
  const int L1 = 0, R1 = 1, W1 = 2, U1 = 3;
  const int L2 = 4, R2 = 5, W2 = 6, U2 = 7, J = 8;
  const int N = ev.size();

  // ---- Program-order edges (MUST, within each thread) ----
  std::vector<std::pair<int, int>> po = {
      {L1, R1}, {R1, W1}, {W1, U1},        // t_fun
      {L2, R2}, {R2, W2}, {W2, U2}, {U2, J} // main
  };

  // Two cases: no sync (different mutexes -> race), or a sync edge (same mutex -> safe).
  // The sync edge is unlock U1 -> lock L2 (same mutex: t_fun releases it, main acquires it).
  for (int has_sync = 0; has_sync <= 1; ++has_sync) {
    z3::context c;
    z3::solver s(c);

    // clock[e] = position of event e in the SC linearization (integer).
    std::vector<z3::expr> clock;
    for (int i = 0; i < N; ++i)
      clock.push_back(c.int_const(("clock_" + ev[i].name).c_str()));

    // All clocks are non-negative.
    for (int i = 0; i < N; ++i)
      s.add(clock[i] >= 0);

    // Program-order: each po edge forces clock(src) < clock(dst).
    for (auto [x, y] : po)
      s.add(clock[x] < clock[y]);

    // Mutex sync (same mutex): unlock U1 happens-before lock L2.
    if (has_sync)
      s.add(clock[U1] < clock[L2]);

    // ---- Race query: can W1 (write in t_fun) and R2 (read in main) be ADJACENT? ----
    // Adjacent = |clock(W1) - clock(R2)| == 1. Push one branch, check satisfiability,
    // then pop and try the other.
    bool any_adjacent = false;
    for (int dir = 0; dir < 2; ++dir) {
      s.push();
      if (dir == 0)
        s.add(clock[W1] == clock[R2] + 1);  // W1 immediately after R2
      else
        s.add(clock[R2] == clock[W1] + 1);  // R2 immediately after W1
      if (s.check() == z3::sat) {
        any_adjacent = true;
        z3::model m = s.get_model();
        std::cout << (has_sync ? "[same-mutex]  " : "[diff-mutex]  ")
                  << "W1/R2 adjacent (dir " << dir << "):";
        for (int i = 0; i < N; ++i)
          std::cout << " " << ev[i].name << "=" << m.eval(clock[i]);
        std::cout << std::endl;
      }
      s.pop();
    }

    std::cout << (has_sync ? "[same-mutex]  " : "[diff-mutex]  ")
              << (any_adjacent ? "SAT  -> RACE" : "UNSAT -> NO RACE")
              << std::endl << std::endl;
  }
  return 0;
}
