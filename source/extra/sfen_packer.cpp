#include "../config.h"

#if defined (USE_SFEN_PACKER)

#include "../misc.h"
#include "../position.h"

#include <sstream>
#include <fstream>
#include <cstring>	// std::memset()

using namespace std;

// -----------------------------------
//        局面の圧縮・解凍
// -----------------------------------

// ビットストリームを扱うクラス
// 局面の符号化を行なうときに、これがあると便利
struct BitStream
{
  // データを格納するメモリを事前にセットする。
  // そのメモリは0クリアされているものとする。
  void  set_data(u8* data_) { data = data_; reset(); }

  // set_data()で渡されたポインタの取得。
  u8* get_data() const { return data; }

  // カーソルの取得。
  int get_cursor() const { return bit_cursor; }

  // カーソルのリセット
  void reset() { bit_cursor = 0; }

  // ストリームに1bit書き出す。
  // bは非0なら1を書き出す。0なら0を書き出す。
  FORCE_INLINE void write_one_bit(int b)
  {
    if (b)
      data[bit_cursor / 8] |= 1 << (bit_cursor & 7);

    ++bit_cursor;
  }

  // ストリームから1ビット取り出す。
  FORCE_INLINE int read_one_bit()
  {
    int b = (data[bit_cursor / 8] >> (bit_cursor & 7)) & 1;
    ++bit_cursor;

    return b;
  }

  // nビットのデータを書き出す
  // データはdの下位から順に書き出されるものとする。
  void write_n_bit(int d, int n)
  {
    for (int i = 0; i < n; ++i)
      write_one_bit(d & (1 << i));
  }

  // nビットのデータを読み込む
  // write_n_bit()の逆変換。
  int read_n_bit(int n)
  {
    int result = 0;
    for (int i = 0; i < n; ++i)
      result |= read_one_bit() ? (1 << i) : 0;

    return result;
  }

private:
  // 次に読み書きすべきbit位置。
  int bit_cursor;

  // データの実体
  u8* data;
};


// Packed position format of Fairy-Stockfish (variant-nnue-tools) for shogi, 512 bits:
//
//   side to move                    1 bit (0 = sente)
//   king squares (sente, gote)      7 bits each
//   board without kings             empty square = 1 bit "0",
//                                   piece = 5 bit code + 1 bit color
//   hand counts (sente, gote)       5 bits for each piece type in fairy_piece_order
//   rest (castling, ep, rule50, move number) is left zero.
//
// Squares are numbered from the bottom left corner as seen from sente (9i = 0, 1i = 8, 9h = 9, ...),
// the board is written from rank "a" to rank "i" and within a rank from file 9 to file 1.
//
// The piece code is 2 * index + 1, where index is the position of the piece type in
// fairy_piece_order (the order of the shogi piece types in Fairy-Stockfish).
// Fairy-Stockfish has no separate promoted minor pieces, so they are written as golds.

static const PieceType fairy_piece_order[] = { BISHOP, ROOK, SILVER, DRAGON, PAWN, LANCE, KNIGHT, GOLD, HORSE, KING };
static const int FAIRY_PIECE_TYPES = 10;

// Maximum number of pieces of a raw piece type, needed to keep the piece numbers of evalList in range.
static const int max_piece_count[KING] = { 0, 18, 4, 4, 4, 2, 2, 4 };

static int fairy_piece_index(PieceType pt)
{
  // Promoted minor pieces are golds in Fairy-Stockfish.
  if (pt == PRO_PAWN || pt == PRO_LANCE || pt == PRO_KNIGHT || pt == PRO_SILVER)
    pt = GOLD;

  for (int i = 0; i < FAIRY_PIECE_TYPES; ++i)
    if (fairy_piece_order[i] == pt)
      return i;

  ASSERT_LV1(false);
  return 0;
}

static int to_fairy_square(Square sq) { return (RANK_9 - rank_of(sq)) * 9 + (FILE_9 - file_of(sq)); }
static Square from_fairy_square(int i) { return (File)(FILE_9 - i % 9) | (Rank)(RANK_9 - i / 9); }

// sfenを圧縮/解凍するためのクラス
struct SfenPacker
{
  // sfenをpackしてdata[64]に格納する。
  void pack(const Position& pos)
  {
    memset(data, 0, 64 /* 512bit */);
    stream.set_data(data);

    // 手番
    stream.write_one_bit((int)(pos.side_to_move()));

    // 先手玉、後手玉の位置、それぞれ7bit
    for (auto c : COLOR)
      stream.write_n_bit(to_fairy_square(pos.king_square(c)), 7);

    // 盤面の駒は王以外はそのまま書き出して良し！
    for (Rank r = RANK_1; r <= RANK_9; ++r)
      for (File f = FILE_9; f >= FILE_1; --f)
      {
        Piece pc = pos.piece_on(f | r);
        if (type_of(pc) == KING)
          continue;
        write_board_piece_to_stream(pc);
      }

    // 手駒は駒種ごとに枚数を書き出す
    for (auto c : COLOR)
      for (PieceType pt : fairy_piece_order)
      {
        int n = (pt == DRAGON || pt == HORSE || pt == KING) ? 0 : hand_count(pos.hand_of(c), pt);
        stream.write_n_bit(n, 5);
      }

    ASSERT_LV3(stream.get_cursor() <= 512);
  }

  // data[64]をsfen化して返す。
  string unpack()
  {
    Piece board[SQ_NB];
    Hand hand[COLOR_NB];
    Color turn;
    if (!decode(board, hand, turn))
      return "";

    return Position::sfen_from_rawdata(board, hand, turn, 0);
  }

  // dataを盤面、手駒、手番に展開する。不正なデータならfalseを返す。
  //
  // Promoted minor pieces are stored as golds, so golds exceeding the four real ones are turned back
  // into promoted pieces of a piece type that is missing otherwise. Which of the golds on the board
  // becomes the promoted piece is arbitrary, the evaluation does not distinguish them
  // (unless DISTINGUISH_GOLDS is defined).
  bool decode(Piece board[SQ_NB], Hand hand[COLOR_NB], Color& turn)
  {
    stream.set_data(data);

    for (auto sq : SQ)
      board[sq] = NO_PIECE;
    hand[BLACK] = hand[WHITE] = HAND_ZERO;

    turn = (Color)stream.read_one_bit();

    for (auto c : COLOR)
    {
      int i = stream.read_n_bit(7);
      if (i >= SQ_NB)
        return false;
      board[from_fairy_square(i)] = make_piece(c, KING);
    }

    for (Rank r = RANK_1; r <= RANK_9; ++r)
      for (File f = FILE_9; f >= FILE_1; --f)
      {
        Square sq = f | r;
        if (type_of(board[sq]) == KING)
          continue;

        if (!stream.read_one_bit())
          continue;

        int index = stream.read_n_bit(4);
        if (index >= FAIRY_PIECE_TYPES || fairy_piece_order[index] == KING)
          return false;

        Color c = (Color)stream.read_one_bit();
        board[sq] = make_piece(c, fairy_piece_order[index]);
      }

    for (auto c : COLOR)
      for (PieceType pt : fairy_piece_order)
      {
        int n = stream.read_n_bit(5);
        if (n == 0)
          continue;
        if (pt == DRAGON || pt == HORSE || pt == KING)
          return false;
        add_hand(hand[c], pt, n);
      }

    // 駒種ごとの枚数
    int count[KING] = {};
    for (auto sq : SQ)
      if (board[sq] != NO_PIECE && type_of(board[sq]) != KING)
        count[raw_type_of(board[sq])]++;
    for (auto c : COLOR)
      for (PieceType pt = PAWN; pt < KING; ++pt)
        count[pt] += hand_count(hand[c], pt);

    // 金が4枚を超えた分は成り駒に戻す
    for (auto sq : SQ)
    {
      if (count[GOLD] <= max_piece_count[GOLD])
        break;

      if (type_of(board[sq]) != GOLD)
        continue;

      for (PieceType pt : { PAWN, LANCE, KNIGHT, SILVER })
        if (count[pt] < max_piece_count[pt])
        {
          board[sq] = make_piece(color_of(board[sq]), (PieceType)(pt + PIECE_TYPE_PROMOTE));
          count[pt]++;
          count[GOLD]--;
          break;
        }
    }

    for (PieceType pt = PAWN; pt < KING; ++pt)
      if (count[pt] > max_piece_count[pt])
        return false;

    return true;
  }

  // pack()でpackされたsfen(512bit = 64bytes)
  // もしくはunpack()でdecodeするsfen
  u8 *data; // u8[64];

  BitStream stream;

  // 盤面の駒をstreamに出力する。
  void write_board_piece_to_stream(Piece pc)
  {
    if (pc == NO_PIECE)
    {
      stream.write_one_bit(0);
      return;
    }

    // 駒種
    stream.write_n_bit(2 * fairy_piece_index(type_of(pc)) + 1, 5);

    // 先後フラグ
    stream.write_one_bit(color_of(pc));
  }
};


// -----------------------------------
//        Positionクラスに追加
// -----------------------------------

// 高速化のために直接unpackする関数を追加。
// packer::decode()とPosition::set()とを合体させて書く。
Tools::Result Position::set_from_packed_sfen(const PackedSfen& sfen , StateInfo * si, Thread* th, bool mirror , int gamePly_ /* = 0 */)
{
	SfenPacker packer;
	packer.data = (u8*)&sfen;

	Piece packed_board[SQ_NB];
	Hand packed_hand[COLOR_NB];
	Color turn;
	if (!packer.decode(packed_board, packed_hand, turn))
		return Tools::Result(Tools::ResultCode::SomeError);

	std::memset(this, 0, sizeof(Position));
	std::memset(si, 0, sizeof(StateInfo));
	st = si;

	// 手番
	sideToMove = turn;

#if defined(USE_EVAL_LIST)

    // evalListのclear。上でmemsetでゼロクリアしたときにクリアされているが…。
	evalList.clear();

	// PieceListを更新する上で、どの駒がどこにあるかを設定しなければならないが、
	// それぞれの駒をどこまで使ったかのカウンター
	PieceNumber piece_no_count[KING] = { PIECE_NUMBER_ZERO,PIECE_NUMBER_PAWN,PIECE_NUMBER_LANCE,PIECE_NUMBER_KNIGHT,
		PIECE_NUMBER_SILVER, PIECE_NUMBER_BISHOP, PIECE_NUMBER_ROOK,PIECE_NUMBER_GOLD };
#endif

	kingSquare[BLACK] = kingSquare[WHITE] = SQ_NB;

	// 盤上の駒
	for (auto sq : SQ)
	{
		Piece pc = packed_board[sq];

		// 駒がない場合もあるのでその場合はスキップする。
		if (pc == NO_PIECE)
			continue;

		if (mirror)
			sq = Mir(sq);

		put_piece(sq, pc);

#if defined(USE_EVAL_LIST)
		// evalListの更新
		PieceNumber piece_no =
			(pc == B_KING) ? PIECE_NUMBER_BKING : // 先手玉
			(pc == W_KING) ? PIECE_NUMBER_WKING : // 後手玉
			piece_no_count[raw_type_of(pc)]++; // それ以外

		evalList.put_piece(piece_no, sq, pc); // sqの升にpcの駒を配置する
#endif
	}

	// 手駒
	hand[BLACK] = packed_hand[BLACK];
	hand[WHITE] = packed_hand[WHITE];

#if defined(USE_EVAL_LIST)
	for (auto c : COLOR)
		for (PieceType pt = PAWN; pt < KING; ++pt)
			for (int i = 0; i < hand_count(hand[c], pt); ++i)
			{
				// FV38などではこの個数分だけpieceListに突っ込まないといけない。
				PieceNumber piece_no = piece_no_count[pt]++;
				ASSERT_LV1(is_ok(piece_no));
				evalList.put_piece(piece_no, c, pt, i);
			}
#endif

	gamePly = gamePly_;

	// put_piece()したのでこのタイミングでupdate
	// set_state()で駒種別のbitboardを参照するのでそれまでにこの関数を呼び出す必要がある。
	update_bitboards();
	update_kingSquare();

	set_state(st);

	// --- effect

#if defined (LONG_EFFECT_LIBRARY)
	// 利きの全計算による更新
	LongEffect::calc_effect(*this);
#endif

	// --- evaluate

	st->materialValue = Eval::material(*this);
	Eval::compute_eval(*this);

	thisThread = th;

	return Tools::Result::Ok();
}

// 盤面と手駒、手番を与えて、そのsfenを返す。
std::string Position::sfen_from_rawdata(Piece board[81], Hand hands[2], Color turn, int gamePly_)
{
  // 内部的な構造体にコピーして、sfen()を呼べば、変換過程がそこにしか依存していないならば
  // これで正常に変換されるのでは…。
  Position pos;

  memcpy(pos.board, board, sizeof(Piece) * 81);
  memcpy(pos.hand, hands, sizeof(Hand) * 2);
  pos.sideToMove = turn;
  pos.gamePly = gamePly_;

  return pos.sfen();

  // ↑の実装、美しいが、いかんせん遅い。
  // 棋譜を大量に読み込ませて学習させるときにここがボトルネックになるので直接unpackする関数を書く。
}

// packされたsfenを得る。引数に指定したバッファに返す。
void Position::sfen_pack(PackedSfen& sfen)
{
  SfenPacker sp;
  sp.data = (u8*)&sfen;
  sp.pack(*this);
}

// packされたsfenを解凍する。sfen文字列が返る。
std::string Position::sfen_unpack(const PackedSfen& sfen)
{
  SfenPacker sp;
  sp.data = (u8*)&sfen;
  return sp.unpack();
}


#endif // USE_SFEN_PACKER

