# fix-rugby18

A small tool that fixes the Rugby 18 achievement "We've Made It!" on Steam by patching the game while it's running.

## Bug

"We've Made It!" (`RUGBY18_ACHIEVEMENT_ID_TITLE_03`) is for being promoted to the top division (Pro D1) in career mode. It never unlocks. Steam statistics show it at less than 1% of players.

The career has 8 divisions. `Data\GD\CareerDescription\Parameters.xmb` lists them from the bottom to the top. Each division has a `PromoUp` value, which is the number of teams that go up at the end of the season:

| Index | Division | PromoUp |
|---|---|---|
| 0 | Division 6 | 1 |
| 1 | Division 5 | 1 |
| 2 | Division 4 | 1 |
| 3 | Division 3 | 2 |
| 4 | Division 2 | 2 |
| 5 | Division 1 | 1 |
| 6 | Pro D2 | 2 |
| 7 | Pro D1 | 0 |

At the end of each season, the game sends an event with a "promoted" flag. The flag is set when your rank is less than or equal to `PromoUp` of your current division. After that, the game moves your team up one division.

The achievement check runs on this event. This is the check, as read from the game code (`Rugby18.exe+0x304300`):

```c
if (event == END_OF_SEASON && promoted) {
	if (division < division_count && rank <= promo_up[division])
		return division + 1 == division_count;
}

return false;
```

`division` is the division that you played in this season, before the promotion. Thus `division + 1 == division_count` is only true in Pro D1, but Pro D1 has `PromoUp` 0, so a team in Pro D1 is never promoted...

The correct check would be `division + 2 == division_count`, you are promoted from Pro D2 into Pro D1. In the game this is one byte: `lea eax, [rsi + 1]` must be `lea eax, [rsi + 2]`.

## What the tool does

1. It finds the running game. If the game is not running, it starts the game through Steam.
2. It waits for the game code to be ready. The game file is encrypted by Steam DRM, so the code is only readable in memory after the game starts.
3. It finds the faulty instruction by its byte pattern, and changes the one byte from `1` to `2`.
4. It reads the byte back to make sure that the change is correct.

The change is in memory only. When you close the game, the fix is gone. Run the tool each time you start the game for a career session where you can get promoted to Pro D1.

## Is it safe?

Probably, the game has no anti-cheat, and we're just patching a single byte in memory.

## How to use

1. Download `fix-rugby18.zip` from the [releases page](https://github.com/Kruithne/fix-rugby18/releases) and extract it.
2. Make sure that Steam is open.
3. Run `fix-rugby18.exe`. You can start the game first, or let the tool start it.
4. Wait for the message `Fixed`. Keep the game open.
5. Play your career. When you are promoted from Pro D2 at the end of the season, the achievement unlocks.

If you are already in Pro D1, you cannot get the achievement in that career. Start a new career, or get relegated to Pro D2 and promoted again.

## Stranger Danger

If you don't trust downloading random executable files from strange people on the internet, that's absolutely reasonable and the correct answer.

The executable found on the releases page is automatically built from the source code in `fix-rugby18.c` via `release.yml`, so you can audit the whole process yourself or ask that one programmer nerd friend you have to take a look.

If you still don't trust it, you can download the .c file yourself and build it. Just run `build.bat`.

You need GCC for Windows (for example [w64devkit](https://github.com/skeeto/w64devkit)).

## Disclaimer

This project is not affiliated with Eko Software, Bigben Interactive or Valve. Use it at your own risk.
