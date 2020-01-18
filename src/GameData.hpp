#pragma once

//reversed engineered data structures

#pragma pack(push)
#pragma pack(1)

constexpr int MaxSoldiersPerSide = 160;
constexpr int MaxTeamsPerSide = 16;
constexpr int MaxVehiclesPerSide = MaxTeamsPerSide;
constexpr int MaxSoldiersPerTeam = 10;

//it might be that first zero of SoldierData, VehicleData and TeamData
//actually belong to previous entry (or unrelated)
//doesn't really matter unless the should be zero assertion fails

struct SoldierData
{
	uint8_t Index;
	uint8_t Something;
	uint8_t IsGerman; //not 100% sure
	uint8_t MustBeZero2;
	char Name[26]; //will be "Unknown" on unused entry
	uint8_t Unknown[148 - 4 - sizeof(Name)];

	void CheckAssumptions()
	{
		//check assumptions
		static_assert(sizeof(*this) == 148, "SoldierData length wrong");
		assert(MustBeZero1 == 0);
		assert(MustBeZero2 == 0);
	}
};

struct VehicleData
{
	uint8_t Index;
	uint8_t Something;
	uint8_t IsGerman; //not 100% sure
	uint8_t MustBeZero2;
	char Name[30]; //will be "Unknown" on unused entry
	uint8_t Unknown[84 - 4 - sizeof(Name)];

	void CheckAssumptions()
	{
		//check assumptions
		static_assert(sizeof(*this) == 84, "VehicleData length wrong");
		assert(MustBeZero1 == 0);
		assert(MustBeZero2 == 0);
	}
};

struct TeamData
{
	uint8_t Index;
	uint8_t Something;
	uint8_t IsGerman; //not 100% sure
	uint8_t MustBeZero2;
	char Name[26]; //will be "Unknown" on unused entry
	uint8_t Type;
	uint8_t MustBeZero1;
	uint16_t Soldiers[MaxSoldiersPerTeam];
	uint8_t VehicleIndex;

	uint8_t Unknown[120 - 4 - sizeof(Name) - sizeof(Type) - sizeof(MustBeZero1) - sizeof(Soldiers) - sizeof(VehicleIndex)];

	void CheckAssumptions()
	{
		//check assumptions
		static_assert(sizeof(*this) == 120, "TeamData length wrong");
		assert(MustBeZero1 == 0);
		assert(MustBeZero2 == 0);
		assert(MustBeZero3 == 0);
	}
};

//should be possible to read battle fields as plain C struct
struct BattleFileData
{
	uint8_t Unknown1[44388];
	SoldierData RussianSoldiers[MaxSoldiersPerSide];
	uint8_t Unknown2[4];
	VehicleData RussianVehicles[MaxVehiclesPerSide];
	uint8_t Unknown5[4];
	TeamData RussianTeams[MaxTeamsPerSide];
	uint8_t Unknown3[4];
	SoldierData GermanSoldiers[MaxSoldiersPerSide];
	uint8_t Unknown4[4];
	VehicleData GermanVehicles[MaxVehiclesPerSide];
	uint8_t Unknown6[4];
	TeamData GermanTeams[MaxTeamsPerSide];
	uint8_t Unknown[106580 - sizeof(Unknown1) - sizeof(RussianSoldiers) - sizeof(Unknown2) - sizeof(RussianTeams) - sizeof(Unknown3) - sizeof(RussianVehicles) - sizeof(Unknown5) - sizeof(GermanSoldiers) - sizeof(Unknown4) - sizeof(GermanVehicles) - sizeof(Unknown6) - sizeof(GermanTeams)];

	void CheckAssumptions()
	{
		static_assert(sizeof(*this) == 106580, "BattleFileData length wrong");
		static_assert(offsetof(BattleFileData, RussianSoldiers) == 44388, "Wrong offset of RussianSoldiers");
		static_assert(offsetof(BattleFileData, RussianVehicles) == 68072, "Wrong offset of RussianVehicles");
		static_assert(offsetof(BattleFileData, RussianTeams) == 69420, "Wrong offset of RussianTeams");
		static_assert(offsetof(BattleFileData, GermanSoldiers) == 71344, "Wrong offset of GermanSoldiers");
		static_assert(offsetof(BattleFileData, GermanVehicles) == 95028, "Wrong offset of GermanVehicles");
		static_assert(offsetof(BattleFileData, GermanTeams) == 96376, "Wrong offset of GermanTeams");

		for (SoldierData& data : RussianSoldiers)
			data.CheckAssumptions();
		for (SoldierData& data : GermanSoldiers)
			data.CheckAssumptions();
		for (VehicleData& data : RussianVehicles)
			data.CheckAssumptions();
		for (VehicleData& data : GermanVehicles)
			data.CheckAssumptions();
		for (TeamData& data : RussianTeams)
			data.CheckAssumptions();
		for (TeamData& data : GermanTeams)
			data.CheckAssumptions();
	}
};

#pragma pack(pop)
