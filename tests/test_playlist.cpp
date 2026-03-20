//
// test_playlist.cpp
//
// Unit tests for CPlaylist
//

#include "doctest/doctest.h"
#include "playlist.h"
#include "stubs/circle/timer.h"

#include <cstring>

namespace StubTimer { extern unsigned s_clock_ticks; }

// ---------------------------------------------------------------
// Empty playlist invariants
// ---------------------------------------------------------------

TEST_CASE("Playlist: empty state")
{
	CPlaylist pl;
	CHECK(pl.IsEmpty());
	CHECK(pl.GetCount() == 0u);
	CHECK(pl.GetCurrent() == nullptr);
	CHECK(pl.GetEntry(0) == nullptr);
	CHECK(pl.GetCurrentIndex() == 0u);
	CHECK(!pl.AdvanceToNext());
	CHECK(!pl.AdvanceToPrev());
}

// ---------------------------------------------------------------
// Add / duplicate / overflow
// ---------------------------------------------------------------

TEST_CASE("Playlist: add and retrieve entries")
{
	CPlaylist pl;
	CHECK(pl.Add("SD:/a.mid"));
	CHECK(pl.Add("SD:/b.mid"));
	CHECK(pl.Add("SD:/c.mid"));
	CHECK(pl.GetCount() == 3u);
	CHECK(!pl.IsEmpty());
	REQUIRE(pl.GetEntry(0) != nullptr);
	CHECK(strcmp(pl.GetEntry(0), "SD:/a.mid") == 0);
	CHECK(strcmp(pl.GetEntry(1), "SD:/b.mid") == 0);
	CHECK(strcmp(pl.GetEntry(2), "SD:/c.mid") == 0);
	CHECK(pl.GetEntry(3) == nullptr);
}

TEST_CASE("Playlist: duplicate rejected")
{
	CPlaylist pl;
	CHECK(pl.Add("SD:/a.mid"));
	CHECK(!pl.Add("SD:/a.mid"));
	CHECK(pl.GetCount() == 1u);
}

TEST_CASE("Playlist: empty/null path rejected")
{
	CPlaylist pl;
	CHECK(!pl.Add(""));
	CHECK(!pl.Add(nullptr));
	CHECK(pl.GetCount() == 0u);
}

// ---------------------------------------------------------------
// Navigation without repeat
// ---------------------------------------------------------------

TEST_CASE("Playlist: AdvanceToNext without repeat")
{
	CPlaylist pl;
	pl.Add("SD:/a.mid");
	pl.Add("SD:/b.mid");
	pl.Add("SD:/c.mid");

	CHECK(pl.GetCurrentIndex() == 0u);
	CHECK(pl.AdvanceToNext());
	CHECK(pl.GetCurrentIndex() == 1u);
	CHECK(strcmp(pl.GetCurrent(), "SD:/b.mid") == 0);
	CHECK(pl.AdvanceToNext());
	CHECK(pl.GetCurrentIndex() == 2u);
	// At the end with repeat off — must not advance
	CHECK(!pl.AdvanceToNext());
	CHECK(pl.GetCurrentIndex() == 2u);
}

TEST_CASE("Playlist: AdvanceToPrev without repeat")
{
	CPlaylist pl;
	pl.Add("SD:/a.mid");
	pl.Add("SD:/b.mid");
	// At index 0, prev must fail
	CHECK(!pl.AdvanceToPrev());
	pl.AdvanceToNext(); // now at 1
	CHECK(pl.AdvanceToPrev());
	CHECK(pl.GetCurrentIndex() == 0u);
}

// ---------------------------------------------------------------
// Navigation with repeat
// ---------------------------------------------------------------

TEST_CASE("Playlist: AdvanceToNext wraps with repeat")
{
	CPlaylist pl;
	pl.SetRepeat(true);
	pl.Add("SD:/a.mid");
	pl.Add("SD:/b.mid");
	pl.AdvanceToNext(); // index 1
	CHECK(pl.AdvanceToNext()); // wraps → index 0
	CHECK(pl.GetCurrentIndex() == 0u);
}

TEST_CASE("Playlist: AdvanceToPrev wraps with repeat")
{
	CPlaylist pl;
	pl.SetRepeat(true);
	pl.Add("SD:/a.mid");
	pl.Add("SD:/b.mid");
	pl.Add("SD:/c.mid");
	// At index 0, prev wraps to last
	CHECK(pl.AdvanceToPrev());
	CHECK(pl.GetCurrentIndex() == 2u);
	CHECK(strcmp(pl.GetCurrent(), "SD:/c.mid") == 0);
}

// ---------------------------------------------------------------
// Remove
// ---------------------------------------------------------------

TEST_CASE("Playlist: Remove reduces count and shifts entries")
{
	CPlaylist pl;
	pl.Add("SD:/a.mid");
	pl.Add("SD:/b.mid");
	pl.Add("SD:/c.mid");
	pl.Remove(1); // remove b
	CHECK(pl.GetCount() == 2u);
	CHECK(strcmp(pl.GetEntry(0), "SD:/a.mid") == 0);
	CHECK(strcmp(pl.GetEntry(1), "SD:/c.mid") == 0);
}

TEST_CASE("Playlist: Remove adjusts current index when entry before current is removed")
{
	CPlaylist pl;
	pl.Add("SD:/a.mid");
	pl.Add("SD:/b.mid");
	pl.Add("SD:/c.mid");
	pl.AdvanceToNext(); // current = 1 (b)
	pl.AdvanceToNext(); // current = 2 (c)
	pl.Remove(0);       // remove a; current was 2, now should be 1 (still c)
	CHECK(pl.GetCurrentIndex() == 1u);
	CHECK(strcmp(pl.GetCurrent(), "SD:/c.mid") == 0);
}

TEST_CASE("Playlist: Remove last entry clamps current index")
{
	CPlaylist pl;
	pl.Add("SD:/a.mid");
	pl.Add("SD:/b.mid");
	pl.AdvanceToNext(); // current = 1
	pl.Remove(1);       // remove current; 1 entry left
	CHECK(pl.GetCount() == 1u);
	CHECK(pl.GetCurrentIndex() == 0u);
}

TEST_CASE("Playlist: Remove invalid index is no-op")
{
	CPlaylist pl;
	pl.Add("SD:/a.mid");
	pl.Remove(5); // out of range
	CHECK(pl.GetCount() == 1u);
}

// ---------------------------------------------------------------
// Clear
// ---------------------------------------------------------------

TEST_CASE("Playlist: Clear empties the queue")
{
	CPlaylist pl;
	pl.Add("SD:/a.mid");
	pl.Add("SD:/b.mid");
	pl.AdvanceToNext();
	pl.Clear();
	CHECK(pl.IsEmpty());
	CHECK(pl.GetCurrentIndex() == 0u);
}

// ---------------------------------------------------------------
// MoveUp / MoveDown
// ---------------------------------------------------------------

TEST_CASE("Playlist: MoveUp swaps entries")
{
	CPlaylist pl;
	pl.Add("SD:/a.mid");
	pl.Add("SD:/b.mid");
	pl.Add("SD:/c.mid");
	CHECK(pl.MoveUp(1)); // b → position 0
	CHECK(strcmp(pl.GetEntry(0), "SD:/b.mid") == 0);
	CHECK(strcmp(pl.GetEntry(1), "SD:/a.mid") == 0);
	CHECK(!pl.MoveUp(0)); // already first — must fail
}

TEST_CASE("Playlist: MoveDown swaps entries")
{
	CPlaylist pl;
	pl.Add("SD:/a.mid");
	pl.Add("SD:/b.mid");
	pl.Add("SD:/c.mid");
	CHECK(pl.MoveDown(1)); // b → position 2
	CHECK(strcmp(pl.GetEntry(1), "SD:/c.mid") == 0);
	CHECK(strcmp(pl.GetEntry(2), "SD:/b.mid") == 0);
	CHECK(!pl.MoveDown(2)); // already last — must fail
}

TEST_CASE("Playlist: MoveUp tracks current index")
{
	CPlaylist pl;
	pl.Add("SD:/a.mid");
	pl.Add("SD:/b.mid");
	pl.AdvanceToNext(); // current = 1 (b)
	pl.MoveUp(1);       // b moves to index 0; current must follow
	CHECK(pl.GetCurrentIndex() == 0u);
	CHECK(strcmp(pl.GetCurrent(), "SD:/b.mid") == 0);
}

TEST_CASE("Playlist: MoveDown tracks current index")
{
	CPlaylist pl;
	pl.Add("SD:/a.mid");
	pl.Add("SD:/b.mid");
	pl.Add("SD:/c.mid");
	// current at 0 (a)
	pl.MoveDown(0);  // a moves to index 1; current must follow
	CHECK(pl.GetCurrentIndex() == 1u);
	CHECK(strcmp(pl.GetCurrent(), "SD:/a.mid") == 0);
}

// ---------------------------------------------------------------
// SetCurrentByPath
// ---------------------------------------------------------------

TEST_CASE("Playlist: SetCurrentByPath finds the correct entry")
{
	CPlaylist pl;
	pl.Add("SD:/a.mid");
	pl.Add("SD:/b.mid");
	pl.Add("SD:/c.mid");
	pl.SetCurrentByPath("SD:/c.mid");
	CHECK(pl.GetCurrentIndex() == 2u);
	CHECK(strcmp(pl.GetCurrent(), "SD:/c.mid") == 0);
}

TEST_CASE("Playlist: SetCurrentByPath no-op on unknown path")
{
	CPlaylist pl;
	pl.Add("SD:/a.mid");
	pl.Add("SD:/b.mid");
	pl.SetCurrentByPath("SD:/unknown.mid");
	CHECK(pl.GetCurrentIndex() == 0u); // unchanged
}

// ---------------------------------------------------------------
// Shuffle — verify count preserved and all entries still present
// ---------------------------------------------------------------

TEST_CASE("Playlist: Shuffle preserves all entries")
{
	StubTimer::s_clock_ticks = 12345u;
	CPlaylist pl;
	pl.Add("SD:/a.mid");
	pl.Add("SD:/b.mid");
	pl.Add("SD:/c.mid");
	pl.Add("SD:/d.mid");

	pl.Shuffle();

	CHECK(pl.GetCount() == 4u);
	CHECK(pl.GetCurrentIndex() == 0u);

	const char* names[] = { "SD:/a.mid", "SD:/b.mid", "SD:/c.mid", "SD:/d.mid" };
	bool found[4] = { false, false, false, false };
	for (unsigned i = 0; i < 4; ++i)
		for (unsigned j = 0; j < 4; ++j)
			if (pl.GetEntry(i) && strcmp(pl.GetEntry(i), names[j]) == 0)
				found[j] = true;

	for (unsigned j = 0; j < 4; ++j)
		CHECK(found[j]);
}

TEST_CASE("Playlist: SetShuffle(true) triggers shuffle immediately")
{
	StubTimer::s_clock_ticks = 99u;
	CPlaylist pl;
	pl.Add("SD:/a.mid");
	pl.Add("SD:/b.mid");
	pl.Add("SD:/c.mid");
	pl.SetShuffle(true);
	CHECK(pl.GetShuffle());
	CHECK(pl.GetCount() == 3u);
	CHECK(pl.GetCurrentIndex() == 0u); // reset to start
}

// ---------------------------------------------------------------
// BuildJSON
// ---------------------------------------------------------------

TEST_CASE("Playlist: BuildJSON basic structure")
{
	CPlaylist pl;
	pl.Add("SD:/a.mid");
	pl.Add("SD:/b.mid");

	char buf[1024];
	const int n = pl.BuildJSON(buf, sizeof(buf));
	REQUIRE(n > 0);
	CHECK(strstr(buf, "\"count\":2") != nullptr);
	CHECK(strstr(buf, "\"index\":0") != nullptr);
	CHECK(strstr(buf, "\"repeat\":false") != nullptr);
	CHECK(strstr(buf, "\"shuffle\":false") != nullptr);
	CHECK(strstr(buf, "SD:/a.mid") != nullptr);
	CHECK(strstr(buf, "SD:/b.mid") != nullptr);
}

TEST_CASE("Playlist: BuildJSON empty playlist")
{
	CPlaylist pl;
	char buf[256];
	const int n = pl.BuildJSON(buf, sizeof(buf));
	REQUIRE(n > 0);
	CHECK(strstr(buf, "\"count\":0") != nullptr);
	CHECK(strstr(buf, "\"entries\":[]") != nullptr);
}
