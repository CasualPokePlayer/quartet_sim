// clang++ -std=c++23 -O3 -march=native -mtune=native quartet.cpp -o quartet.exe

#define _CRT_SECURE_NO_WARNINGS
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <list>
#include <semaphore>
#include <thread>
#include <utility>

struct QuartetState
{
	uint16_t RngState; // [0xFF80-0xFF81]
	uint8_t FaceType;
	uint8_t FaceProgress; // bitwise flags for corner types
	uint8_t NextFaceState; // [0xFFB2], lower 4 bits is the corner type, upper 4 bits is the face type
	uint8_t TimerSpeed; // [0xFF91]
	uint16_t TurnCount;
	uint16_t TurnDelay0; // can be over 255
	uint8_t TurnDelays[2863]; // [0] is TurnDelay0, don't use
	uint32_t TotalDelay;
};

struct QuartetThreadArgs
{
	uint16_t seed;
	std::counting_semaphore<16>* threadWaitSema;
};

static consteval auto CreateTimerDelayTable()
{
	std::array<uint8_t, 0x81> timerDelayTable{};
	for (uint8_t i = 0x40; i <= 0x80; i++)
	{
        if (i & 1)
        {
            continue;
        }

		uint8_t timer = 0x14;
		uint16_t subTimer = 0;
		while (true)
		{
			subTimer += i;
			if (subTimer >= 0x100)
			{
				subTimer -= 0x100;
				timer--;
				if (timer == 0)
				{
					break;
				}
			}

			timerDelayTable[i]++;
		}

		// 0-indexing vs 1-indexing fixup...
		timerDelayTable[i]--;
	}

	return timerDelayTable;
}

static std::array<uint8_t, 0x81> TimerDelayTable = CreateTimerDelayTable();

static uint8_t GetTimerDelay(QuartetState& quartet)
{
	return TimerDelayTable[quartet.TimerSpeed];
}

/*
static uint8_t AdvanceRng(QuartetState& quartet)
{
	uint8_t regA, regB;
	bool carry, prevCarry;

	// ldh a,[0x81]
	regA = quartet.RngState[1];
	// srl a
	carry = regA & 1;
	regA >>= 1;
	// ld b,a
	regB = regA;
	// ldh a,[0x80]
	regA = quartet.RngState[0];
	// rra
	prevCarry = carry;
	carry = regA & 1;
	regA >>= 1;
	regA |= prevCarry ? 0x80 : 0;
	// ldh [0x80],a
	quartet.RngState[0] = regA;
	// sbc a
	regA -= regA;
	regA -= carry;
	// and a,0xB4
	regA &= 0xB4;
	// xor b
	regA ^= regB;
	// ldh [0x81],a
	quartet.RngState[1] = regA;
	return regA;
}
*/

static consteval std::array<uint16_t, 0xFFFF> CreateRngTable()
{
	// rng has a period of 65535 across all seeds (except 0, which is invalid)
	std::array<uint16_t, 0xFFFF> rngTable{};

	uint16_t rngState = 1;
	for (uint16_t i = 0; i < 0xFFFF; i++)
	{
		rngTable[i] = rngState;
		// same as AdvanceRng, but smaller (better for consteval)
		// note rngState == RngState[0] | RngState[1] << 8
		// i.e. rngState == [0xFF80] | [0xFF81] << 8
		uint16_t xorVal = (rngState & 1) ? 0xB400 : 0;
        rngState = (rngState >> 1) ^ xorVal;
	}

	return rngTable;
}

static consteval std::array<uint16_t, 0x10000> CreateReverseRngTable()
{
	std::array<uint16_t, 0x10000> reverseRngTable{};

	uint16_t rngState = 1;
	for (uint16_t i = 0; i < 0xFFFF; i++)
	{
		reverseRngTable[rngState] = i;
		uint16_t xorVal = (rngState & 1) ? 0xB400 : 0;
        rngState = (rngState >> 1) ^ xorVal;
	}

	return reverseRngTable;
}

static std::array<uint16_t, 0xFFFF> RngTable = CreateRngTable();
static std::array<uint16_t, 0x10000> ReverseRngTable = CreateReverseRngTable();

static void AdvanceRng(QuartetState& quartet, uint32_t iterations = 1)
{
	uint32_t rngIndex = ReverseRngTable[quartet.RngState];
	rngIndex += iterations;
	if (rngIndex >= 0xFFFF)
	{
		rngIndex -= 0xFFFF;
	}

	quartet.RngState = RngTable[rngIndex];
	//return quartet.RngState >> 8;
}

static void FindNextFace(QuartetState& quartet)
{
	AdvanceRng(quartet);
	quartet.NextFaceState = quartet.RngState >> 8;
	quartet.NextFaceState &= 0x33;

	// rng is advanced an extra time once the next face is loaded
	AdvanceRng(quartet);
}

static bool HitDelayLimit(const QuartetState& branch)
{
	// this is the shortest total delay that has been found so far
	//return branch.TotalDelay > 6921;
	return branch.TotalDelay > 6884;
}

static bool ShouldCullBranch(const QuartetState& branch)
{
	// need to match the given face type
	// need to need to not match same corner
	uint8_t nextFaceType = branch.NextFaceState >> 4;
	uint8_t nextFaceCorner = 1 << (branch.NextFaceState & 0xF);
	return nextFaceType != branch.FaceType || (nextFaceCorner & branch.FaceProgress) || HitDelayLimit(branch);
}

static void CullBranches(std::list<QuartetState>& branches)
{
	std::erase_if(branches, ShouldCullBranch);
}

static bool CompareBranches(const QuartetState& branchA, const QuartetState& branchB)
{
	return branchA.TotalDelay < branchB.TotalDelay;
}

static void MergeBranches(std::list<QuartetState>& to, std::list<QuartetState>& from)
{
	// to is always empty, therefore does not need to be sorted
	from.sort(CompareBranches);
	to.merge(from, CompareBranches);
}

static void RemoveDuplicates(std::list<QuartetState>& branches)
{
	// MergeBranches would be called before which would sort branches
	branches.unique([](const QuartetState& branchA, const QuartetState& branchB)
	{
		return branchA.RngState == branchB.RngState &&
			branchA.FaceType == branchB.FaceType &&
			branchA.FaceProgress == branchB.FaceProgress &&
			branchA.NextFaceState == branchB.NextFaceState &&
			branchA.TotalDelay == branchB.TotalDelay;
	});
}

static void SearchSeed(QuartetThreadArgs args)
{
	std::list<QuartetState> branches{};
	std::list<QuartetState> pendingBranches{};

	QuartetState initialState{};
	// args.seed is 0xFF80 << 8 | 0xFF81
	// RngState is 0xFF81 << 8 | 0xFF80
	initialState.RngState = std::byteswap(args.seed);
	initialState.TimerSpeed = 0x40; // initial speed

	// can't delay the first face piece
	FindNextFace(initialState);
	initialState.FaceType = initialState.NextFaceState >> 4;
	initialState.FaceProgress = 1 << (initialState.NextFaceState & 0xF);

	// rng is advanced 5 times before the first input can be made
	// rng is advanced 10 times after the input before the next face is rolled
	AdvanceRng(initialState, 5 + 10);

	pendingBranches.push_back(initialState);

	// first input is special, as it can be delayed for quite a bit of time due to the initial countdown
	// if no inputs end up being done, you get effectively a delay of 260 frames
	for (uint16_t i = 1; i <= 260; i++)
	{
		AdvanceRng(initialState);
		initialState.TurnDelay0 = i;
		initialState.TotalDelay = i; // don't add, as we reuse this branch (and this is turn 0 anyways)
		pendingBranches.push_back(initialState);
	}

	for (auto& pendingBranch : pendingBranches)
	{
		FindNextFace(pendingBranch);
	}

	CullBranches(pendingBranches);
	MergeBranches(branches, pendingBranches);

	for (auto& branch : branches)
	{
		branch.FaceProgress |= 1 << (branch.NextFaceState & 0xF);
		branch.TurnCount++;
	}

	for (uint16_t i = 1; i <= 2862; i++)
	{
		//fprintf(stderr, "Iteration %d, branch count: %zu\n", i, branches.size());

		// once two pieces are in place, we have a current (correct) piece and a future (correct) piece
		// these together make 1 complete face, which case the next piece selected can be any piece
		bool selectingNextNewFace = (i & 3) == 3;

		auto branchIt = branches.begin();
		while (branchIt != branches.end())
		{
			QuartetState& branch = *branchIt;
			// there's 14 frames before input can be done
			// if we're not doing a new face, we have to do a consecutive input
			// which means the first frame of input cannot be used (so forcing an extra frame of delay before input)
			// after the input, it takes 16 frames before the next face is rolled
			// also, if we clear a board, there's 100 or 101 extra frames of delay (depending on if the board clear gives 800 points or a 1-up)
			// timer countdown is dependent on the amount of pieces in place and the amount of board clears in place (ultimately ranging from 78 to 38 frames)
			uint8_t delayLimit;
			switch (i & 3)
			{
				case 0: // board clear (3 corners -> 4/0 corners)
					AdvanceRng(branch, 14 + 1 + 16 + (i <= 12 ? 101 : 100));
					delayLimit = GetTimerDelay(branch) - 1;
					break;
				case 1: // new face (0 corners -> 1 corner)
					AdvanceRng(branch, 14 + 16);
					delayLimit = GetTimerDelay(branch);
					break;
				case 2: // (1 corner -> 2 corners)
					AdvanceRng(branch, 14 + 1 + 16);
					delayLimit = GetTimerDelay(branch) - 1;
					break;
				case 3: // selecting new face (2 corners -> 3 corners)
					AdvanceRng(branch, 14 + 1 + 16);
					delayLimit = GetTimerDelay(branch) - 1;
					break;
				default:
					std::unreachable();
			}

			for (uint8_t j = 0; j <= delayLimit; j++)
			{
				// hack to prevent memory blowing up
				/*if (pendingBranches.size() >= 250000 * 10)
				{
					break;
				}*/

				// make a copy to operate on
				QuartetState pendingBranch = branch;
				pendingBranch.TurnDelays[i] = j;
				pendingBranch.TotalDelay += j;
				FindNextFace(pendingBranch);

				// if we aren't selecting a new face, we might need to cull the branch
				if (selectingNextNewFace)
				{
					// we still obey delay limits when selecting a new face
					if (!HitDelayLimit(pendingBranch))
					{
						pendingBranches.push_back(std::move(pendingBranch));
					}
				}
				else
				{
					if (!ShouldCullBranch(pendingBranch))
					{
						pendingBranches.push_back(std::move(pendingBranch));
					}
				}

				AdvanceRng(branch);
			}

			branchIt = branches.erase(branchIt);
		}

		MergeBranches(branches, pendingBranches);

		// try to remove identical state branches
		// these are effectively duplicate branches here
		RemoveDuplicates(branches);

		for (auto& branch : branches)
		{
			// board cleared, decrease timer speed
			if ((i & 3) == 0)
			{
				branch.TimerSpeed -= 6;
			}

			if (selectingNextNewFace)
			{
				branch.FaceType = branch.NextFaceState >> 4;
				branch.FaceProgress = 0;
			}

			branch.FaceProgress |= 1 << (branch.NextFaceState & 0xF);
			branch.TurnCount++;

			if (branch.TimerSpeed < 0x80)
			{
				branch.TimerSpeed += 2;
			}
		}
	}

	fprintf(stderr, "Branch seed %04X finished\n", args.seed);
	if (branches.size() > 0)
	{
		// dump the results!
		FILE* f = fopen(std::format("seed_0x{:04X}.txt", args.seed).c_str(), "w");
		// branches should still be sorted
		QuartetState& shortestBranch = branches.front();

		fprintf(f, "%s", "{");
		fprintf(f, " 0x%04X", shortestBranch.TurnDelay0);
		uint16_t totalDelay = shortestBranch.TurnDelay0;
		for (uint16_t i = 1; i <= 2862; i++)
		{
			fprintf(f, " 0x%04X", shortestBranch.TurnDelays[i]);
			totalDelay += shortestBranch.TurnDelays[i];
		}

		fprintf(f, " } : 0x%04X\n", totalDelay);
		fclose(f);
	}


	args.threadWaitSema->release();
}

static uint16_t GetSeed(FILE* f)
{
	char seedBuf[5]{};
	if (fread(seedBuf, sizeof(char), sizeof(seedBuf), f) == 0)
	{
		// 0 isn't a valid seed
		return 0;
	}

	seedBuf[4] = '\0';
	return strtol(seedBuf, NULL, 16);
}

int main(void)
{
	std::counting_semaphore<16>* threadWaitSema = new std::counting_semaphore<16>{0};
	FILE* f = fopen("known_seeds_sorted.txt", "rb");
	fseek(f, 0, SEEK_SET);

	unsigned numThreads = std::max(std::thread::hardware_concurrency() / 2, 1u);
	//unsigned numThreads = 1;
	for (unsigned i = 0; i < numThreads; i++)
	{
		QuartetThreadArgs args{GetSeed(f), threadWaitSema};
		std::thread t(SearchSeed, args);
		t.detach();
	}

	while (true)
	{
		uint16_t seed = GetSeed(f);
		if (seed == 0)
		{
			break;
		}

		// wait for free thread
		threadWaitSema->acquire();

		QuartetThreadArgs args{seed, threadWaitSema};
		std::thread t(SearchSeed, args);
		t.detach();
	}

	// wait for all remaining threads
	for (unsigned i = 0; i < numThreads; i++)
	{
		threadWaitSema->acquire();
	}

	delete threadWaitSema;
	return 0;
}
