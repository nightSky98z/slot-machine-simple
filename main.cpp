#include <array>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>

/// 全体の実装方針
/* ゲーム用スロットマシンの基本アルゴリズムは
 * ランダム抽選
 *    ↓
 * リール表示
 *    ↓
 * 結果判定
 *    ↓
 * 報酬計算です。
 */

/* [プレイヤーが回す]
 *    ↓
 * [乱数で結果を決める]
 *    ↓
 * [リールを回転アニメーション]
 *    ↓
 * [停止]
 *    ↓
 * [絵柄を判定]
 *    ↓
 * [当たりなら報酬]
 */

/*SlotMachine
 * ├─ BetManager      // 賭け金
 * ├─ ReelSystem      // リール表示・停止
 * ├─ RandomSystem    // 抽選
 * ├─ JudgeSystem     // 当たり判定
 * └─ RewardSystem    // 報酬付与
 */

/**
 * @brief スロットの絵柄。
 *
 * @note Caller:
 * - enum の値順は重み配列と一致させる。
 * - 値を追加した場合は
 * kSymbolCount、重み、倍率、表示名を更新する。
 */
enum class Symbol
{
  Cherry,
  Bell,
  Seven,
  Bar,
  Lemon,
};

/**
 * @brief ゲーム全体の状態。
 *
 * @note Caller:
 * - Lobby はスロットマシン未入場状態。
 * - Slot は SlotMachine owner が存在する状態。
 */
enum class GameMode
{
  Lobby, // スロットマシンに入っていない
  Slot,  // スロットマシン中
};

inline constexpr std::int32_t kStartMoney = 1000;
inline constexpr std::int32_t kCompleteMoney = 10000;
inline constexpr std::int32_t kDefaultBet = 50;

inline constexpr std::size_t kReelCount = 3;
inline constexpr std::size_t kStripLength = 10;
inline constexpr std::size_t kVisibleRows = 3;
inline constexpr std::size_t kCenterRow = 1;
inline constexpr std::size_t kSymbolCount = 5;

inline constexpr std::size_t kReelStripCount = kReelCount * kStripLength;
inline constexpr std::size_t kVisibleSymbolCount = kReelCount * kVisibleRows;

inline constexpr std::int32_t kCherryWeight = 30;
inline constexpr std::int32_t kBellWeight = 20;
inline constexpr std::int32_t kSevenWeight = 5;
inline constexpr std::int32_t kBarWeight = 10;
inline constexpr std::int32_t kLemonWeight = 35;

inline constexpr std::int32_t kTwoSameMultiplier = 1;
inline constexpr std::int32_t kCherryMultiplier = 10;
inline constexpr std::int32_t kBellMultiplier = 5;
inline constexpr std::int32_t kSevenMultiplier = 120;
inline constexpr std::int32_t kBarMultiplier = 30;
inline constexpr std::int32_t kLemonMultiplier = 2;

using ReelStrips = std::array<Symbol, kReelStripCount>;
using VisibleSymbols = std::array<Symbol, kVisibleSymbolCount>;
using StopIndices = std::array<std::size_t, kReelCount>;
using SymbolWeights = std::array<std::int32_t, kSymbolCount>;

inline constexpr SymbolWeights kSymbolWeights{
    kCherryWeight, kBellWeight, kSevenWeight, kBarWeight, kLemonWeight,
};

/**
 * @brief Symbol
 * の重み合計をコンパイル時に計算する。
 *
 * @return 重み合計。
 */
constexpr std::int32_t symbol_weight_total()
{
  std::int32_t total = 0;

  for (const std::int32_t weight : kSymbolWeights) {
    total += weight;
  }

  return total;
}

static_assert(kReelCount == 3, "This judge rule expects exactly 3 reels");
static_assert(kVisibleRows == 3, "This visible layout expects exactly 3 rows");
static_assert(kCenterRow < kVisibleRows,
              "Center row must be inside visible rows");
static_assert(kStripLength >= kVisibleRows,
              "Strip length must cover visible rows");
static_assert(symbol_weight_total() == 100, "Symbol weights must sum to 100");
static_assert(kStripLength > 0, "Strip length must be greater than 0");

/**
 * @brief 1回のスピン結果。
 *
 * @note Caller:
 * - stop_indices は各リールの中央行に来る strip
 * index。
 * - visible は row-major。index は
 * visible_index(row, reel) で作る。
 */
struct SpinResult
{
  StopIndices stop_indices{};
  VisibleSymbols visible{};
  std::int32_t payout = 0;
};

/**
 * @brief ゲーム全体の状態。
 *
 * @note Caller:
 * - money はスロット外の所持金。
 * - GameMode::Slot の間は SlotMachine owner
 * が存在する必要がある。
 */
struct GameState
{
  std::int32_t money = kStartMoney;
  GameMode mode = GameMode::Lobby;
  bool is_complete = false;
  bool is_game_over = false;
  bool is_running = true;
};

/**
 * @brief Symbol の表示名を返す。
 *
 * @param[in] symbol 表示対象の Symbol。
 * @return 静的寿命を持つ C 文字列。
 */
static const char* symbol_name(const Symbol symbol)
{
  switch (symbol) {
  case Symbol::Cherry:
    return "Cherry";
  case Symbol::Bell:
    return "Bell";
  case Symbol::Seven:
    return "Seven";
  case Symbol::Bar:
    return "Bar";
  case Symbol::Lemon:
    return "Lemon";
  }

  assert(false && "Unknown Symbol value");
  return "Unknown";
}

/**
 * @brief 1次元配列上のリール内部 index を作る。
 */
static std::size_t reel_strip_index(const std::size_t reel,
                                    const std::size_t strip_index)
{
  assert(reel < kReelCount);
  assert(strip_index < kStripLength);

  return reel * kStripLength + strip_index;
}

/**
 * @brief 1次元配列上の表示 index を作る。
 */
static std::size_t visible_index(const std::size_t row, const std::size_t reel)
{
  assert(row < kVisibleRows);
  assert(reel < kReelCount);
  return row * kReelCount + reel;
}

/**
 * @brief リールを輪として扱うため、index
 * を範囲内へ戻す。
 *
 * @param[in] index 基準 index。
 * @param[in] offset index
 * に加える相対位置。今回の用途では -1, 0, +1。
 * @param[in] length 輪として扱う配列長。
 * @return 0 <= result < length を満たす index。
 *
 * @note Caller:
 * - length は 0 より大きい必要がある。
 * - index は length 未満である必要がある。
 * - 現在の実装は offset が -1, 0, +1
 * の範囲であることを前提にする。
 */
static std::size_t wrap_index(const std::size_t index,
                              const std::int32_t offset,
                              const std::size_t length)
{
  assert(length > 0);
  assert(index < length);
  assert(offset >= -1);
  assert(offset <= 1);

  if (offset == -1) {
    if (index == 0) {
      return length - 1;
    }

    return index - 1;
  }

  if (offset == 0) {
    return index;
  }

  if (offset == 1) {
    if (index + 1 == length) {
      return 0;
    }

    return index + 1;
  }

  return index;
}

/**
 * @brief 賭け金とスロット内 credit を管理する。
 *
 * @note Caller:
 * - credit は SlotMachine 内だけで変更する。
 * - スロット退出時は cash_out() 経由で
 * GameState::money に戻す。
 */
class BetManager
{
public:
  BetManager(const std::int32_t initial_credit, const std::int32_t bet)
      : credit_(initial_credit), bet_(bet)
  {
    assert(initial_credit >= 0);
    assert(bet > 0);
  }

  std::int32_t credit() const
  {
    return credit_;
  }

  std::int32_t bet() const
  {
    return bet_;
  }

  bool can_bet() const
  {
    return credit_ >= bet_;
  }

  bool consume_bet()
  {
    if (!can_bet()) {
      return false;
    }

    credit_ -= bet_;
    assert(credit_ >= 0);
    return true;
  }

  void add_credit(const std::int32_t amount)
  {
    assert(amount >= 0);
    credit_ += amount;
  }

  std::int32_t cash_out()
  {
    const std::int32_t returned_credit = credit_;
    credit_ = 0;
    return returned_credit;
  }

private:
  std::int32_t credit_ = 0; // 所持コイン
  std::int32_t bet_ = 0;
};

/**
 * @brief
 * リール生成と停止位置生成に使う乱数を所有する。
 *
 * @note Caller:
 * - 初期リール生成は SlotMachine
 * 初期化時だけ行う。
 * - スピン時は各リールの中央停止 index
 * だけを生成する。
 */
class RandomSystem
{
public:
  explicit RandomSystem(const std::uint32_t seed) : rng_(seed) {}

  Symbol weighted_random_symbol()
  {
    std::discrete_distribution<std::size_t> dist(kSymbolWeights.begin(),
                                                 kSymbolWeights.end());

    const std::size_t symbol_index = dist(rng_);

    assert(symbol_index < kSymbolCount);
    return static_cast<Symbol>(symbol_index);
  }

  ReelStrips make_random_reel_strips()
  {
    ReelStrips strips{};

    for (std::size_t reel = 0; reel < kReelCount; ++reel) {
      for (std::size_t strip_index = 0; strip_index < kStripLength;
           ++strip_index) {
        strips[reel_strip_index(reel, strip_index)] = weighted_random_symbol();
      }
    }

    return strips;
  }

  std::size_t random_stop_index()
  {
    std::uniform_int_distribution<int> dist(0, kStripLength - 1);
    const std::size_t stop_index = dist(rng_);

    assert(stop_index < kStripLength);
    return stop_index;
  }

  StopIndices make_stop_indices()
  {
    StopIndices stop_indices{};

    for (std::size_t reel = 0; reel < kReelCount; ++reel) {
      stop_indices[reel] = random_stop_index();
    }

    return stop_indices;
  }

private:
  std::mt19937 rng_;
};

/**
 * @brief
 * リール配列を所有し、停止結果を表示用配列へ変換する。
 *
 * @note Caller:
 * - strips_ は SlotMachine
 * のライフタイム中だけ有効。
 * - visible は row-major の 1次元配列として扱う。
 */
class ReelSystem
{
public:
  explicit ReelSystem(const ReelStrips& initial_strips)
      : strips_(initial_strips)
  {
  }

  VisibleSymbols make_visible_symbols(const StopIndices& stop_indices) const
  {
    VisibleSymbols visible{};

    for (std::size_t reel = 0; reel < kReelCount; ++reel) {
      const std::size_t center_index = stop_indices[reel];
      assert(center_index < kStripLength);

      if (center_index >= kStripLength) {
        continue;
      }

      const std::size_t top_index = wrap_index(center_index, -1, kStripLength);
      const std::size_t mid_index = wrap_index(center_index, 0, kStripLength);
      const std::size_t bottom_index =
          wrap_index(center_index, 1, kStripLength);

      visible[visible_index(0, reel)] =
          strips_[reel_strip_index(reel, top_index)];

      visible[visible_index(1, reel)] =
          strips_[reel_strip_index(reel, mid_index)];

      visible[visible_index(2, reel)] =
          strips_[reel_strip_index(reel, bottom_index)];
    }

    return visible;
  }

  void show_reel_strips() const
  {
    std::printf("Generated reel strips:\n");

    for (std::size_t reel = 0; reel < kReelCount; ++reel) {
      std::printf("Reel %zu: ", reel);

      for (int strip_index = 0; strip_index < kStripLength; ++strip_index) {
        const Symbol symbol = strips_[reel_strip_index(reel, strip_index)];
        std::printf("%s ", symbol_name(symbol));
      }

      std::printf("\n");
    }
  }

  void show_all_reels(const VisibleSymbols& visible) const
  {
    for (std::size_t row = 0; row < kVisibleRows; ++row) {
      for (std::size_t reel = 0; reel < kReelCount; ++reel) {
        const Symbol symbol = visible[visible_index(row, reel)];
        std::printf("%-8s ", symbol_name(symbol));
      }

      std::printf("\n");
    }
  }

  void show_stop_from_left(const VisibleSymbols& visible) const
  {
    std::printf("Stop reels from left:\n");

    for (std::size_t stopped_reel = 0; stopped_reel < kReelCount;
         ++stopped_reel) {
      std::printf("\nStopped reel: %zu\n", stopped_reel);

      for (std::size_t row = 0; row < kVisibleRows; ++row) {
        for (std::size_t reel = 0; reel < kReelCount; ++reel) {
          if (reel <= stopped_reel) {
            const Symbol symbol = visible[visible_index(row, reel)];
            std::printf("%-8s ", symbol_name(symbol));
          } else {
            std::printf("%-8s ", "Spin");
          }
        }

        std::printf("\n");
      }
    }
  }

private:
  ReelStrips strips_{};
};

/**
 * @brief 表示結果の中央行から payout を計算する。
 */
class JudgeSystem
{
public:
  std::int32_t calc_payout(const VisibleSymbols& visible,
                           const std::int32_t bet) const
  {
    assert(bet > 0);

    const Symbol first = visible[visible_index(kCenterRow, 0)];
    const Symbol mid = visible[visible_index(kCenterRow, 1)];
    const Symbol last = visible[visible_index(kCenterRow, 2)];

    const bool all_same = first == mid && mid == last;

    // 同じものが３つ
    if (all_same) {
      return bet * symbol_multiplier(first);
    }

    // 同じものが２つ
    const bool two_same = first == mid || mid == last || first == last;

    if (two_same) {
      return bet * kTwoSameMultiplier;
    }

    return 0;
  }

private:
  static std::int32_t symbol_multiplier(const Symbol symbol)
  {
    switch (symbol) {
    case Symbol::Seven:
      return kSevenMultiplier;
    case Symbol::Bar:
      return kBarMultiplier;
    case Symbol::Cherry:
      return kCherryMultiplier;
    case Symbol::Bell:
      return kBellMultiplier;
    case Symbol::Lemon:
      return kLemonMultiplier;
    }

    assert(false && "Unknown Symbol value");
    return 0;
  }
};

/**
 * @brief payout をスロット内 credit に反映する。
 */
struct RewardSystem
{
  void apply_payout(BetManager& bet_manager, const std::int32_t payout) const
  {
    assert(payout >= 0);
    bet_manager.add_credit(payout);
  }
};

/**
 * @brief スロットマシン 1
 * 台分の状態と処理を所有する。
 *
 * @note Caller:
 * - GameState::mode == Slot の間だけ存在させる。
 * - 退出時は cash_out() で credit を回収してから
 * owner を reset() する。
 */
class SlotMachine
{
public:
  SlotMachine(const std::int32_t initial_credit, const std::int32_t bet,
              const std::uint32_t seed)
      : bet_manager_{initial_credit, bet}, random_system_(seed),
        reel_system_(random_system_.make_random_reel_strips())
  {
    assert(initial_credit >= bet);
    assert(bet > 0);
  }

  std::int32_t credit() const
  {
    return bet_manager_.credit();
  }

  std::int32_t bet() const
  {
    return bet_manager_.bet();
  }

  bool can_spin() const
  {
    return bet_manager_.can_bet();
  }

  std::int32_t cash_out()
  {
    return bet_manager_.cash_out();
  }

  void show_reel_strips() const
  {
    reel_system_.show_reel_strips();
  }

  bool spin_once()
  {
    std::printf("\nCredit before spin: %d\n", bet_manager_.credit());
    std::printf("Bet: %d\n\n", bet_manager_.bet());

    if (!bet_manager_.consume_bet()) {
      std::printf("Not enough credit.\n");
      return false;
    }

    SpinResult result{};

    result.stop_indices = random_system_.make_stop_indices();
    result.visible = reel_system_.make_visible_symbols(result.stop_indices);

    reel_system_.show_stop_from_left(result.visible);

    std::printf("\nFinal result:\n");
    reel_system_.show_all_reels(result.visible);

    result.payout =
        judge_system_.calc_payout(result.visible, bet_manager_.bet());
    reward_system_.apply_payout(bet_manager_, result.payout);

    std::printf("\nPayout: %d\n", result.payout);
    std::printf("Credit after spin: %d\n", bet_manager_.credit());

    return true;
  }

private:
  BetManager bet_manager_;
  RandomSystem random_system_;
  ReelSystem reel_system_;
  JudgeSystem judge_system_;
  RewardSystem reward_system_;
};

/**
 * @brief スロット内 credit を money
 * に戻し、SlotMachine owner を破棄する。
 *
 * @param[in,out] game money と mode
 * を更新するゲーム状態。
 * @param[in,out] slot_machine SlotMachine
 * の独占所有権。
 *
 * @return money に戻した credit。slot_machine
 * が空なら 0。
 * @post game.mode == GameMode::Lobby。
 *
 * @note Caller:
 * - 通常は GameMode::Slot かつ slot_machine !=
 * nullptr で呼ぶ。
 * - release build でも安全に戻れるように runtime
 * check を残す。
 */
static std::int32_t
exit_slot_machine(GameState& game, std::unique_ptr<SlotMachine>& slot_machine)
{
  assert(game.mode == GameMode::Slot);
  assert(slot_machine != nullptr);

  if (!slot_machine) {
    game.mode = GameMode::Lobby;
    return 0;
  }

  const std::int32_t returned_credit = slot_machine->cash_out();
  game.money += returned_credit;

  slot_machine.reset();
  game.mode = GameMode::Lobby;

  return returned_credit;
}

/**
 * @brief 現在の総資産がコンプリート条件を満たすか判定する。
 */
static bool is_complete_money(const GameState& game,
                              const std::unique_ptr<SlotMachine>& slot_machine)
{
  std::int32_t total_money = game.money;

  if (slot_machine) {
    total_money += slot_machine->credit();
  }

  return total_money >= kCompleteMoney;
}

/**
 * @brief Slot 状態の不変条件を検査する。
 *
 * @note Caller:
 * - 内部状態の検査用。ユーザー入力エラーの処理には使わない。
 */
static void
assert_slot_state_valid(const GameState& game,
                        const std::unique_ptr<SlotMachine>& slot_machine)
{
  if (game.mode == GameMode::Slot) {
    assert(slot_machine != nullptr);
  }

  if (slot_machine) {
    assert(game.mode == GameMode::Slot);
  }
}

/**
 * @brief stdin から std::int32_t を1つ読む。
 *
 * @param[out] out_value 読み取った整数を書き込む。
 * @return 読み取りと変換に成功したら true。
 *
 * @note Caller:
 * - ユーザー入力は外部入力なので assert では処理しない。
 * - 失敗時、out_value の値は使わない。
 */
static bool read_int32_from_stdin(std::int32_t& out_value)
{
  char buffer[64]{};

  if (std::fgets(buffer, sizeof(buffer), stdin) == nullptr) {
    return false;
  }

  const std::size_t length = std::strlen(buffer);
  if (length == 0) {
    return false;
  }

  const char* begin = buffer;
  const char* end = buffer + length;

  // 改行は数値変換の対象外にする。
  if (length > 0 && buffer[length - 1] == '\n') {
    end = buffer + length - 1;
  }

  std::int32_t value = 0;
  const std::from_chars_result result = std::from_chars(begin, end, value);

  if (result.ec != std::errc{}) {
    return false;
  }

  // "123abc" のような入力を拒否する。
  if (result.ptr != end) {
    return false;
  }

  out_value = value;
  return true;
}

/**
 * @brief stdin から 1 文字コマンドを読む。
 *
 * @param[out] out_command 読み取ったコマンド文字を書き込む。
 * @return 読み取りに成功し、有効な 1 文字だけだった場合 true。
 *
 * @note Caller:
 * - ユーザー入力は外部入力なので assert では処理しない。
 * - "s" や "q" は受け付ける。
 * - "spin" のような複数文字入力は拒否する。
 */
static bool read_command_from_stdin(char& out_command)
{
  char buffer[32]{};

  if (std::fgets(buffer, sizeof(buffer), stdin) == nullptr) {
    return false;
  }

  std::size_t index = 0;

  while (buffer[index] == ' ' || buffer[index] == '\t') {
    ++index;
  }

  const char command = buffer[index];

  if (command == '\0' || command == '\n') {
    return false;
  }

  ++index;

  while (buffer[index] == ' ' || buffer[index] == '\t') {
    ++index;
  }

  if (buffer[index] != '\0' && buffer[index] != '\n') {
    return false;
  }

  out_command = command;
  return true;
}

/**
 * @brief 有効な 1 文字コマンドを読めるまで再入力を求める。
 *
 * @param[out] out_command 読み取ったコマンド文字を書き込む。
 * @return 読み取りに成功したら true。EOF なら false。
 *
 * @note Caller:
 * - ユーザー入力失敗は通常起こり得るため assert しない。
 * - EOF は入力継続不能なので false を返す。
 */
static bool read_command_retry(char& out_command)
{
  while (true) {
    if (read_command_from_stdin(out_command)) {
      return true;
    }

    if (std::feof(stdin)) {
      return false;
    }

    std::printf("Input error. Please input one command character.\n");
    std::printf("> ");
  }
}

/**
 * @brief Lobby 状態のコマンドを処理する。
 */
static void update_lobby(GameState& game,
                         std::unique_ptr<SlotMachine>& slot_machine,
                         std::random_device& seed, const char command)
{
  assert(game.mode == GameMode::Lobby);
  assert(slot_machine == nullptr);

  if (command != 's') {
    std::printf("Unknown command.\n");
    return;
  }

  if (game.money < kDefaultBet) {
    std::printf("Not enough money to start slot machine.\n");
    std::printf("Game Over.\n");
    game.is_game_over = true;
    game.is_running = false;
    return;
  }

  std::int32_t credit = 0;
  std::printf("いくらクレジットに換金しますか(1:1)？ (所持金: %d): ",
              game.money);

  if (!read_int32_from_stdin(credit)) {
    std::printf("Input error.\n");
    game.is_running = false;
    return;
  }

  if (credit <= 0) {
    std::printf("Credit must be positive.\n");
    return;
  }

  if (credit > game.money) {
    std::printf("Not enough money.\n");
    return;
  }

  if (credit < kDefaultBet) {
    std::printf("Credit must be >= bet. Bet: %d\n", kDefaultBet);
    return;
  }

  game.money -= credit;
  slot_machine = std::make_unique<SlotMachine>(
      credit, kDefaultBet, static_cast<std::uint32_t>(seed()));

  slot_machine->show_reel_strips();
  game.mode = GameMode::Slot;
  assert_slot_state_valid(game, slot_machine);
  std::printf("Enter slot machine.\n");
}

/**
 * @brief Slot 状態のコマンドを処理する。
 */
static void update_slot(GameState& game,
                        std::unique_ptr<SlotMachine>& slot_machine,
                        const char command)
{
  assert(game.mode == GameMode::Slot);
  assert(slot_machine != nullptr);

  if (!slot_machine) {
    std::printf("Slot state error: slot machine is missing.\n");
    game.mode = GameMode::Lobby;
    game.is_running = false;
    return;
  }

  if (command == 'e') {
    const std::int32_t returned_credit = exit_slot_machine(game, slot_machine);

    std::printf("Exit slot machine.\n");
    std::printf("Returned credit: %d\n", returned_credit);
    std::printf("Money: %d\n", game.money);

    if (game.money >= kCompleteMoney) {
      std::printf("Game Complete!\n");
      game.is_complete = true;
      game.is_running = false;
    }

    return;
  }

  if (command != 's') {
    std::printf("Unknown command.\n");
    return;
  }

  if (!slot_machine->can_spin()) {
    const std::int32_t returned_credit = exit_slot_machine(game, slot_machine);

    std::printf("Credit is less than bet.\n");
    std::printf("Auto exit slot machine.\n");
    std::printf("Returned credit: %d\n", returned_credit);
    std::printf("Money: %d\n", game.money);

    if (game.money < kDefaultBet) {
      std::printf("Game Over.\n");
      game.is_game_over = true;
      game.is_running = false;
    }

    return;
  }

  static_cast<void>(slot_machine->spin_once());

  if (is_complete_money(game, slot_machine)) {
    const std::int32_t returned_credit = exit_slot_machine(game, slot_machine);
    static_cast<void>(returned_credit);

    std::printf("Game Complete!\n");
    std::printf("Final Money: %d\n", game.money);

    game.is_complete = true;
    game.is_running = false;
    return;
  }

  if (!slot_machine->can_spin()) {
    const std::int32_t returned_credit = exit_slot_machine(game, slot_machine);

    std::printf("Credit is less than bet.\n");
    std::printf("Auto exit slot machine.\n");
    std::printf("Returned credit: %d\n", returned_credit);
    std::printf("Money: %d\n", game.money);

    if (game.money < kDefaultBet) {
      std::printf("Game Over.\n");
      game.is_game_over = true;
      game.is_running = false;
    }
  }
}

int main()
{
  std::random_device seed;

  GameState game{};
  std::unique_ptr<SlotMachine> slot_machine;

  while (game.is_running) {
    assert_slot_state_valid(game, slot_machine);

    std::printf("\n==============================\n");
    std::printf("Money: %d\n", game.money);

    if (game.mode == GameMode::Lobby) {
      assert(slot_machine == nullptr);

      std::printf("State: Lobby\n");
      std::printf("Command: s = start slot, q = "
                  "quit game\n");
    } else {
      assert(game.mode == GameMode::Slot);
      assert(slot_machine != nullptr);

      if (!slot_machine) {
        std::printf("Slot state error: slot machine is missing.\n");
        break;
      }

      std::printf("State: Slot\n");
      std::printf("Credit: %d\n", slot_machine->credit());
      std::printf("Bet: %d\n", slot_machine->bet());
      std::printf("Command: s = spin, e = exit "
                  "slot, q = quit game\n");
    }

    std::printf("> ");

    char command = 0;

    if (!read_command_retry(command)) {
      std::printf("Input closed.\n");
      break;
    }

    if (command == 'q') {
      if (slot_machine) {
        game.money += slot_machine->cash_out();
        slot_machine.reset();
      }

      game.mode = GameMode::Lobby;
      std::printf("Quit game.\n");
      game.is_running = false;
      break;
    }

    if (game.mode == GameMode::Lobby) {
      update_lobby(game, slot_machine, seed, command);
    } else {
      update_slot(game, slot_machine, command);
    }
  }
  return 0;
}
