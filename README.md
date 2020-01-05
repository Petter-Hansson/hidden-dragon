# hidden-dragon
A Close Combat 3: Cross of Iron bot

## Project Aim
Develop a strategic AI that is more fun than the painfully bad AI in Close Combat 3: Cross of Iron, mostly because I don't have time for multiplayer (if anyone even plays anymore) and a game that was last patched around 2010 is unlikely to receive more patches. The primary issue with CoI AI is that it doesn't recognize when it's inferior (which is likely to be the case after a few battles against human in a campaign) and doesn't stay in ambush. Instead, it runs around merrily, getting blasted immediately by properly deployed forces, sometimes having to request truce 1 min into battle. It's also a bit faster to give up by fleeing than necessary results in even fewer casualities for the human player.

## How to Run
- Run the bot executable.
- Press OK in the dialog box if you want to automatically find the DirectPlay session on your local network (recommended). Otherwise, you can type in an IP or hostname to search.
- Start CC3.exe and enter multiplayer.
- Press TCP/IP, the game will freeze looking for games (there won't be any).
- Press Host. Make sure you are the Germans. DON'T PRESS READY YET. DON'T CHANGE FROM DEFAULT SCENARIO in current version.
- The bot should join after ~10 seconds if it can find the game on your network.
- Press Ready - the bot will do the same instantly.
- Press Next and enter the requesition screen. You can do changes here, the bot won't.
- Press Next again to enter deployment. The bot will finish deployment near instantly, you can take your time.
- Start the battle. The bot is currently braindead and won't do anything but may be harder to fight than the shipped Cross of Iron AI anyway. ;-)
- The game will hang if you attempt to flee or (I suspect, I haven't bothered yet) win the battle. Just escape before that happens.

## How to Compile
The game relies on the DX7 SDK which may not be the easiest to find anymore, but I did by googling around a bit. I haven't bothered optimizing the project dependencies yet for that matter. All in due time.

## Project Status
Currently in pre-alpha. The bot can join when you host a game (ON THE SAME computer) via enumerating sessions on your local method or otherwise an explicitly stated IP address, exclusively as the Russians on the default scenario. This will likely remain for a while because quite some decoding is necessary. Here is a list of steps necessary:

## Architecture
- The bot relies on ancient DirectPlay to join your multiplayer game as an impostor client. The game believes the bot is actually a normal client.
- The memory of the CC3.exe process will (eventually) be read to avoid having to reconstruct the gamestate which would be near impossible and take a hundred man years or more.
- I went into this project assuming Close Combat 3 used RTS lock step networking. It kind of does, but not entirely. It may necessary to reverse engineer the full unit requisition logic in order for the bot to even join a different scenario than the default one.

## Why "Hidden Dragon"?
It's a cheesy name that seemed suitable from a certain cheesy movie.