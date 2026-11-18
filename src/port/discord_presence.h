#ifndef LAKE_TRIO_DISCORD_PRESENCE_H
#define LAKE_TRIO_DISCORD_PRESENCE_H

#include <stdint.h>

/* Native desktop-only, no Discord SDK or third-party binary dependency. */
void DiscordPresence_Tick(uint32_t milliseconds);
void DiscordPresence_SetLocation(const char *location);
void DiscordPresence_ClearLocation(void);
void DiscordPresence_SetBattle(const char *details, const char *matchup);
void DiscordPresence_ClearBattle(void);

#endif
