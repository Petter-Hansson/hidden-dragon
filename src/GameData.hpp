#pragma once

//reversed engineered data structures

#pragma pack(push)
#pragma pack(1)

constexpr int MaxSoldiersPerSide = 160;

//len 148
//#160 per team
//Russian team comes after German
//TODO: verify from running battle, it seems that Russians are uninitialized with the name Unknown in German requisition
//implying it's necessary to read battle files to reconstruct things
struct SoldierData
{
	uint8_t MustBeZero1;
	uint8_t Index;
	uint8_t IsRussian; //not 100% sure
	uint8_t IsGerman; //not 100% sure
	uint8_t MustBeZero2;
	char Name[26]; //will be "Unknown" on unused entry
	uint8_t Unknown[148 - 5 - sizeof(Name)];

	void CheckAssumptions()
	{
		//check assumptions
		static_assert(sizeof(*this) == 148, "SoldierData length wrong");
		assert(MustBeZero1 == 0);
		assert(MustBeZero2 == 0);
	}
};

//should be possible to read battle fields as plain C struct
struct BattleFileData
{
	uint8_t Unknown1[44387];
	SoldierData RussianSoldiers[MaxSoldiersPerSide];
	uint8_t Unknown2[3276];
	SoldierData GermanSoldiers[MaxSoldiersPerSide];
	uint8_t Unknown[106580 - sizeof(Unknown1) - sizeof(RussianSoldiers) - sizeof(Unknown2) - sizeof(GermanSoldiers)];

	void CheckAssumptions()
	{
		static_assert(sizeof(*this) == 106580, "BattleFileData length wrong");
		static_assert(offsetof(BattleFileData, RussianSoldiers) == 44387, "Wrong offset of RussianSoldiers");
		static_assert(offsetof(BattleFileData, GermanSoldiers) == 71343, "Wrong offset of GermanSoldiers");

		for (SoldierData& data : RussianSoldiers)
			data.CheckAssumptions();
		for (SoldierData& data : GermanSoldiers)
			data.CheckAssumptions();
	}
};

#pragma pack(pop)
