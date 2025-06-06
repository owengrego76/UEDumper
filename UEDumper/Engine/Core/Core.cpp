#include "Core.h"

#include "FName_decryption.h"
#include "../UEClasses/UnrealClasses.h"
#include "../Userdefined/StructDefinitions.h"
#include "Frontend/Windows/LogWindow.h"
#include "Frontend/Windows/PackageViewerWindow.h"
#include "Settings/EngineSettings.h"


//https://github.com/EpicGames/UnrealEngine/blob/4.25/Engine/Source/Runtime/Core/Private/UObject/UnrealNames.cpp
//https://github.com/EpicGames/UnrealEngine/blob/4.25/Engine/Source/Runtime/Core/Public/UObject/NameTypes.h
//FName::ToString
//https://github.com/EpicGames/UnrealEngine/blob/5.1/Engine/Source/Runtime/Core/Private/UObject/UnrealNames.cpp#L3375
//https://github.com/EpicGames/UnrealEngine/blob/release/Engine/Source/Runtime/Core/Private/UObject/UnrealNames.cpp#L251


//flag some invalid characters in a name
std::string generateValidVarName(const std::string& str)
{
	//flag some invalid characters in a name
	std::string result = "";
	for (const char c : str)
	{
		if (static_cast<int>(c) < 0 || !std::isalnum(c))
			result += '_';
		else
			result += c;

	}

	return result;
};

//we always compare this function to FName::ToString(FString& Out) in the source code
std::string EngineCore::FNameToString(FName fname)
{
	if (FNameCache.contains(fname.ComparisonIndex))
	{
		return FNameCache[fname.ComparisonIndex];
	}

	//unreal engine 4.19 - 4.22 fname read function
#if UE_VERSION < UE_4_23

	// the game doesnt use chunks so its completely different
	// lets take a look at the func at
	// https://github.com/EpicGames/UnrealEngine/blob/4.19/Engine/Source/Runtime/Core/Private/UObject/UnrealNames.cpp#L942
	// we first get how the FNameEntry is determined and then how FNameEntry::AppendNameToString( FString& String ) works which
	// returns the plain string.
	// lets look at how the FNameEntry is determined. The ToString function calls FName::GetDisplayNameEntry() to return a FNameEntry*
	// and the func is at
	// https://github.com/EpicGames/UnrealEngine/blob/4.19/Engine/Source/Runtime/Core/Private/UObject/UnrealNames.cpp#L919
	// that gets the Names array and the index via GetDisplayIndex() which is at
	// https://github.com/EpicGames/UnrealEngine/blob/4.19/Engine/Source/Runtime/Core/Public/UObject/NameTypes.h#L579
	// which just calls GetDisplayIndexFast() and returns the DisplayIndex or ComparisonIndex depending on WITH_CASE_PRESERVING_NAME. See
	// https://github.com/EpicGames/UnrealEngine/blob/4.19/Engine/Source/Runtime/Core/Public/UObject/NameTypes.h#L1192
	// now lets determine the Names array. The Names array is a TNameEntryArray. But what is TNameEntryArray? Its defined at
	// https://github.com/EpicGames/UnrealEngine/blob/4.19/Engine/Source/Runtime/Core/Public/UObject/NameTypes.h#L489
	// which is a TStaticIndirectArrayThreadSafeRead thats defined at
	// https://github.com/EpicGames/UnrealEngine/blob/4.19/Engine/Source/Runtime/Core/Public/UObject/NameTypes.h#L342
	// where we look at the function that returns the pointer at
	// https://github.com/EpicGames/UnrealEngine/blob/4.19/Engine/Source/Runtime/Core/Public/UObject/NameTypes.h#L391
	// we see it takes a Index as param like the one from GetDisplayIndex() and does following:
	// int32 ChunkIndex = Index / ElementsPerChunk;
	// int32 WithinChunkIndex = Index % ElementsPerChunk;
	// where ElementsPerChunk is 16384 or 0x4000.
	// and gets the ElementType** Chunk = Chunks[ChunkIndex]; (ElementType is FNameEntry)
	// where the Chunks array is just the gnames array.
	// the gnames array holds pointers to the FNameEntries
	// the following gets returned: Chunk + WithinChunkIndex;
	// this will be our FNameEntry**!
	// now lets go to FNameEntry::AppendNameToString( FString& String ) which is at
	// https://github.com/EpicGames/UnrealEngine/blob/4.19/Engine/Source/Runtime/Core/Private/UObject/UnrealNames.cpp#L126
	// which just returns the WideName or AnsiName
	// which is at FNameEntry at offset 0x16 looking at
	// https://github.com/EpicGames/UnrealEngine/blob/4.19/Engine/Source/Runtime/Core/Public/UObject/NameTypes.h#L138
	// thats it, we just need to get the bytes there!
	// the representative code:
	constexpr auto ElementsPerChunk = 0x4000;

	enum { NAME_SIZE = 1024 };

	char name[NAME_SIZE + 1] = { 0 };

#if WITH_CASE_PRESERVING_NAME
	const int32_t Index = fname.DisplayIndex;
#else
	const int32_t Index = fname.ComparisonIndex;
#endif

	const int32_t ChunkIndex = Index / ElementsPerChunk;
	const int32_t WithinChunkIndex = Index % ElementsPerChunk;

	//GNAMES_POOL_OFFSET exists as theres always a offset for whatever reason. Check this in IDA!!!!!!!!!
	const uint64_t ElementType = Memory::read<uint64_t>(gNames + 8 * ChunkIndex + GNAMES_POOL_OFFSET);

	//WithinChunkIndex * 8 as its full of pointers
	const auto FNameEntryPtrPtr = ElementType + (WithinChunkIndex * 8);

	const auto FNameEntryPtr = Memory::read<uint64_t>(FNameEntryPtrPtr);

	//read the bytes
#if UE_VERSION == UE_4_22
	const uint64_t AnsiName = FNameEntryPtr + 0xC;
#else
	const uint64_t AnsiName = FNameEntryPtr + 0x10;
#endif


	Memory::read(
		reinterpret_cast<void*>(AnsiName),
		name,
		NAME_SIZE
	);

#else // >= 4_23

	enum { NAME_SIZE = 1024 };

	char name[NAME_SIZE + 1] = { 0 };

	//>4.23 name chunks exist
	const unsigned int chunkOffset = fname.ComparisonIndex >> 16; //HIWORD
	const unsigned short nameOffset = fname.ComparisonIndex; //unsigned __int16


	//average function since 4.25
	//https://github.com/EpicGames/UnrealEngine/blob/5.1/Engine/Source/Runtime/Core/Private/UObject/UnrealNames.cpp#L3375

#if WITH_CASE_PRESERVING_NAME
	uint64_t namePoolChunk = Memory::read<uint64_t>(gNames + 8 * (chunkOffset + 2)) + 4 * nameOffset;

	const auto nameLength = Memory::read<uint16_t>(namePoolChunk + 4) >> 1;

	if (nameLength > NAME_SIZE)
	{
		// we're about to corrupt our memory in the next call to Memory::read if we don't clamp the value!
		windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_ERROR, "CORE", "Memory corruption avoided! FName nameLength > NAME_SIZE! Setting WITH_CASE_PRESERVING_NAME=TRUE might resolve this issue");
		puts("Memory corruption avoided! FName nameLength > NAME_SIZE! Setting WITH_CASE_PRESERVING_NAME=TRUE might resolve this issue");
		//DebugBreak();
	}

	Memory::read(
		reinterpret_cast<void*>(namePoolChunk + 6), 
		name, 
		// safeguard against overflow and memory corruption
		nameLength < NAME_SIZE ? nameLength : NAME_SIZE
	);
#else
	int64_t namePoolChunk = Memory::read<uint64_t>(gNames + 8 * (chunkOffset + 2)) + 2 * nameOffset;

	const auto nameLength = Memory::read<uint16_t>(namePoolChunk) >> 6;

	if (nameLength > NAME_SIZE)
	{
		// we're about to corrupt our memory in the next call to Memory::read if we don't clamp the value!
		windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_ERROR, "CORE", "Memory corruption avoided! FName nameLength > NAME_SIZE! Setting WITH_CASE_PRESERVING_NAME=TRUE might resolve this issue");
		puts("Memory corruption avoided! FName nameLength > NAME_SIZE! Setting WITH_CASE_PRESERVING_NAME=TRUE might resolve this issue");
		//DebugBreak();
	}

	Memory::read(
		reinterpret_cast<void*>(namePoolChunk + 2),
		name,
		// safeguard against overflow and memory corruption
		nameLength < NAME_SIZE ? nameLength : NAME_SIZE
	);
#endif

#endif

#if USE_FNAME_ENCRYPTION
	//decrypt the FNames buffer
	fname_decrypt(name, nameLength);
#endif


	std::string finalName = std::string(name);

	if (finalName.empty())
		finalName = "null";
	//throw std::runtime_error("empty name is trying to get cached");

	FNameCache.insert(std::pair(fname.ComparisonIndex, std::string(name)));

	return finalName;
}

uint64_t EngineCore::getOffsetAddress(const Offset& offset)
{
	if (!offset)
		return 0;

	if (offset.flag & OFFSET_SIGNATURE)
	{
		return Memory::patternScan(offset.flag, offset.sig, offset.mask);
	}
	if (offset.flag & OFFSET_ADDRESS)
	{
		return Memory::getBaseAddress() + offset.offset;
	}
	return 0;
}

Offset EngineCore::getOffsetForName(const std::string& name)
{
	for (auto& offset : offsets)
	{
		if (offset.name == name)
			return offset;
	}
	return{};
}

std::vector<Offset> EngineCore::getOffsets()
{
	return offsets;
}

bool EngineCore::generateFNameFile(int& progressDone, int& totalProgress)
{
	//the master header contains all the imports sorted
	std::ofstream FNameFile(EngineSettings::getWorkingDirectory() / "FNames.txt");

	totalProgress = FNameCache.size();

	std::vector<std::pair<int, std::string>> sortedNames;

	for (const auto& pair : FNameCache)
	{
		sortedNames.emplace_back(pair);
	}

	// Sort the vector based on integer keys
	std::ranges::sort(sortedNames);

	FNameFile << "FName dump generated by UEDumper by Spuckwaffel.\n\n\n";

	for (const auto& pair : sortedNames)
	{
		progressDone++;
		char buff[2000] = { 0 };
		if (pair.second.length() > 1900)
		{
			FNameFile << "Name for id " << pair.first << "too long!\n";
			continue;
		}

		sprintf_s(buff, sizeof(buff), "[%05d] %s", pair.first, pair.second.c_str());
		FNameFile << buff << std::endl;
	}
	FNameFile.close();

	progressDone = totalProgress;

	return true;
}

bool EngineCore::generateStructOrClass(UStruct* object, std::vector<EngineStructs::Struct>& data)
{
	//this struct is completely useless
	if (object->PropertiesSize == 0) return false;

	EngineStructs::Struct eStruct;
	eStruct.memoryAddress = object->objectptr;
	eStruct.size = object->PropertiesSize;
	eStruct.minAlignment = object->MinAlignment;
	/*
	* The purpose of maxSize is to calculate the true 'max' size of a class.
	* Since size = object->PropertiesSize, this is what UE reports as the size of the object
	* *including padding*!
	* Therefore maxSize is the 'real' max size without the padding based on our calculations
	* of subclasses having members at offsets less than the reported size of the super.
	*/
	//set this as the current max size, but it will get overridden
	eStruct.maxSize = eStruct.size;
	eStruct.fullName = object->getFullName();
	eStruct.cppName = object->getCName();

	//supers?
	if (object->SuperStruct)
	{
		if (const auto super = object->getSuper())
		{
			for (auto& obj : object->getAllSupers())
			{
				//add them to a vector
				eStruct.superNames.push_back(obj->getCName());
			}
			eStruct.inherited = true;
		}

	}

#if UE_VERSION < UE_4_25
	if (object->Children)
	{

		for (auto child = object->getChildren(); child; child = child->getNext())
		{
			if (ObjectsManager::CRITICAL_STOP_CALLED())
				return false;

			if (!child || !child->IsA<UProperty>())
				continue;

			auto prop = child->castTo<UProperty>();
			EngineStructs::Member member;
			member.size = prop->ElementSize * prop->ArrayDim;
			member.arrayDim = prop->ArrayDim;
			member.name = generateValidVarName(prop->getName());
			//should not happen
			if (member.size == 0)
			{
				windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_ONLY_LOG, "CORE", "member %s size is 0! ", member.name.c_str());
				//DebugBreak();
				continue;
			}
			auto type = prop->getType();
			member.type = type;
			member.offset = prop->getOffset();

			if (type.propertyType == PropertyType::Unknown)
			{
				windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_ONLY_LOG, "CORE", "Struct %s: %s at 0x%p is unknown prop! Missing support?", object->getCName().c_str(), member.name.c_str(), member.offset);
				continue;
			}
			if (type.propertyType == PropertyType::BoolProperty && prop->castTo<UBoolProperty>()->isBitField())
			{
				auto boolProp = child->castTo<UBoolProperty>();

				const auto bitPos = boolProp->getBitPosition(boolProp->ByteMask);
				member.isBit = true;
				member.bitOffset = bitPos;
			}
			eStruct.definedMembers.push_back(member);
		}
	}

#else


	if (object->ChildProperties)
	{
		for (auto child = object->getChildProperties(); child; child = child->getNext())
		{
			EngineStructs::Member member;
			member.size = child->ElementSize * child->ArrayDim;
			member.arrayDim = child->ArrayDim;
			member.name = generateValidVarName(child->getName());

			if (member.size == 0)
			{
				windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_ONLY_LOG, "CORE", "member %s size is 0! ", member.name.c_str());
				//DebugBreak();
				continue;
			}

			auto type = child->getType();
			member.type = type;

			member.offset = child->getOffset();

			if (type.propertyType == PropertyType::Unknown)
			{
				windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_ONLY_LOG, "CORE", "Struct %s: %s at offset 0x%llX is unknown prop! Missing support?", object->getCName().c_str(), member.name.c_str(), member.offset);
				continue;
			}

			if (type.propertyType == PropertyType::BoolProperty && child->castTo<FBoolProperty>()->isBitField())
			{
				auto boolProp = child->castTo<FBoolProperty>();

				const auto bitPos = boolProp->getBitPosition(boolProp->ByteMask);

				member.isBit = true;
				member.bitOffset = bitPos;
			}
			eStruct.definedMembers.push_back(member);
		}
	}
#endif
	// get struct functions
	generateFunctions(object, eStruct.functions);
	data.push_back(eStruct);

	return true;
}

template<typename T>
constexpr uint64_t GetMaxOfType()
{
	return (1ull << (sizeof(T) * 0x8ull)) - 1;
}

std::string getEnumTypeFromSize(const int size)
{
	if (size > sizeof(int32_t)) return TYPE_UI64;
	if (size > sizeof(int16_t)) return TYPE_UI32;
	if (size > sizeof(int8_t)) return TYPE_UI16;

	return TYPE_UI8;
}

int getEnumSizeFromType(const std::string type)
{
	if (type == TYPE_UI64) return sizeof(uint64_t);
	if (type == TYPE_UI32) return sizeof(uint32_t);
	if (type == TYPE_UI16) return sizeof(uint16_t);
	if (type == TYPE_UI8) return sizeof(uint8_t);

	return 0;
}

std::string setEnumSizeForValue(uint64_t EnumValue)
{
	if (EnumValue > GetMaxOfType<uint32_t>())
		return TYPE_UI64;
	if (EnumValue > GetMaxOfType<uint16_t>())
		return TYPE_UI32;
	if (EnumValue > GetMaxOfType<uint8_t>())
		return TYPE_UI16;
	return TYPE_UI8;
}

bool EngineCore::generateEnum(const UEnum* object, std::vector<EngineStructs::Enum>& data)
{
	EngineStructs::Enum eEnum;
	eEnum.fullName = object->getFullName();
	eEnum.memoryAddress = object->objectptr;

	int64_t maxNum = 0;

	const auto names = object->getNames();

	if (!names.size())
		return false;

	for (int i = 0; i < names.size(); i++)
	{
		auto& name = names[i];
		if (name.GetValue() > maxNum && i != names.size() - 1) maxNum = name.GetValue();

		auto fname = FNameToString(name.GetKey());
		std::ranges::replace(fname, ':', '_');

		if (!fname.ends_with("_MAX"))
			eEnum.members.push_back(std::pair(fname, name.GetValue()));
	}
	eEnum.type = setEnumSizeForValue(maxNum);
	eEnum.size = getEnumSizeFromType(eEnum.type);

	eEnum.cppName = object->getName();

	data.push_back(eEnum);

	return true;
}


bool EngineCore::generateFunctions(const UStruct* object, std::vector<EngineStructs::Function>& data)
{

#if UE_VERSION < UE_4_25
	if (!object->Children)
		return false;
#else
	if (!object->Children)
		return false;

#endif

	//i am so sorry for the indent here but fucking reSharper from intellij is so bad and fucks up the
	//indenting for the entire rest of the core.cpp file just because some #define shit
	//this made me so mad i couldnt care less the code misses now a indent

	//in every version we have to go through the children to
for (auto fieldChild = object->getChildren(); fieldChild; fieldChild = fieldChild->getNext())
{
	if (ObjectsManager::CRITICAL_STOP_CALLED())
		return false;

	if (!fieldChild || !fieldChild->IsA<UFunction>())
		continue;


	const auto fn = fieldChild->castTo<UFunction>();

	EngineStructs::Function eFunction;
	eFunction.fullName = fn->getFullName();
	eFunction.cppName = fn->getName();
	eFunction.memoryAddress = fn->objectptr;
	eFunction.functionFlags = fn->getFunctionFlagsString();
	eFunction.binaryOffset = fn->Func - Memory::getBaseAddress();

#if UE_VERSION < UE_4_25

	//ue < 4.25 uses the children but we have to cast them to a UProperty to use the flags
	for (auto child = fn->getChildren(); child; child = child->getNext())
	{
		const auto propChild = child->castTo<UProperty>();
#else

	//ue >= 4.25 we go through the childproperties and we dont have to cast as they are already FProperties
	for (auto child = fn->getChildProperties(); child; child = child->getNext())
	{
		const auto propChild = child;

#endif

		//rest of the code is identical, nothing changed here
		const auto propertyFlags = propChild->PropertyFlags;

		if (propertyFlags & EPropertyFlags::CPF_ReturnParm && !eFunction.returnType)
			eFunction.returnType = propChild->getType();
		else if (propertyFlags & EPropertyFlags::CPF_Parm)
		{
			eFunction.params.push_back(std::tuple(propChild->getType(), propChild->getName(), propertyFlags, propChild->ArrayDim));
		}
	}

	// no defined return type => void
	if (!eFunction.returnType)
		eFunction.returnType = { false, PropertyType::StructProperty, "void" };

	data.push_back(eFunction);
}
	return true;
}

bool EngineCore::RUNAddMemberToMemberArray(EngineStructs::Struct & eStruct, const EngineStructs::Member & newMember)
{
	//basic 0(1) checks before iterating

	//below class base offset? 
	if (newMember.offset < eStruct.getInheritedSize())
	{
		windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_WARNING, "CORE", "Add member failed: offset 0x%X is below base class offset 0x%X!", newMember.offset, eStruct.getInheritedSize());
		return false;
	}
	//above class?
	if (newMember.offset > eStruct.size)
	{
		windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_WARNING, "CORE", "Add member failed: offset 0x%X is greater than class size 0x%X!", newMember.offset, eStruct.size);
		return false;
	}
	//offset + size larger than class size?
	if (newMember.offset + newMember.size > eStruct.size)
	{
		windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_WARNING, "CORE", "Add member failed: offset 0x%X with size %d is greater than class size 0x%X!", newMember.offset + newMember.size, eStruct.size);
		return false;
	}

	//larger than class size? Thats weird and will only happen if offset is negative otherwise handled by above
	if (newMember.size > eStruct.size - eStruct.getInheritedSize())
	{
		windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_WARNING, "CORE", "Add member failed: member is too large for class (%d / %d)", newMember.size, eStruct.size - eStruct.getInheritedSize());
		return false;
	}

	//empty? Mostly the case if the struct has just a unknownmember and nothing defined
	if (eStruct.definedMembers.size() == 0)
	{
		//nothing really needed to check
		eStruct.definedMembers.push_back(newMember);
	}

	for (int i = 0; i < eStruct.definedMembers.size(); i++)
	{
		const auto& nextMember = eStruct.definedMembers[i];

		//check if its smaller than the next member
		if (newMember.offset < nextMember.offset)
		{
			//is the new member somehow interferring the next member
			if (newMember.offset + newMember.size > nextMember.offset)
			{
				windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_WARNING, "CORE", "Add member failed: member is interferring other member (0x%X -> 0x%X -!- 0x%X)", newMember.offset, newMember.offset + newMember.size, nextMember.offset);
				return false;
			}
			//yep we found place
			eStruct.definedMembers.insert(eStruct.definedMembers.begin() + i, newMember);
			return true;
		}
		//occurence can only happen if both members are a bit. if not, error
		if (newMember.offset == nextMember.offset)
		{
			//only allowed if they are bits
			if (!newMember.isBit || !nextMember.isBit)
			{
				windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_WARNING, "CORE", "Add member failed: attempted to override a existing member (one of them is not a bit) (isBit: %d isBit %d)", newMember.isBit, nextMember.isBit);
				return false;
			}
			if (newMember.bitOffset == nextMember.bitOffset)
			{
				windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_WARNING, "CORE", "Add member failed: attempted to override a existing member (both have the same botOffset) (%d, %d)", newMember.bitOffset, nextMember.bitOffset);
				return false;
			}
			if (newMember.bitOffset < nextMember.bitOffset)
			{
				//yep we found place
				eStruct.definedMembers.insert(eStruct.definedMembers.begin() + i, newMember);
				return true;
			}
			//other cases like > are skipped, they should be checked in the next iter
		}
		//all cases should be handled in the next iteration as the member is larger

		//exception: this was the last iteration
		if (i == eStruct.definedMembers.size() - 1)
		{
			//sizes etc are already checked before the loop
			eStruct.definedMembers.push_back(newMember);
			return true;
		}
	}
	//will never be reached
	return false;
}

void EngineCore::cookMemberArray(EngineStructs::Struct & eStruct)
{
	//clear the existing array
	if (!eStruct.cookedMembers.empty())
		eStruct.cookedMembers.clear();


	auto checkRealMemberSize = [&](EngineStructs::Member* currentMember)
	{
		//set the real size
		if (!currentMember->type.isPointer())
		{
			if (const auto classObject = getInfoOfObject(currentMember->type.name))
			{
				if (classObject->type == ObjectInfo::OI_Struct || classObject->type == ObjectInfo::OI_Class)
				{
					const auto cclass = static_cast<EngineStructs::Struct*>(classObject->target);
					if(!cclass->noFixedSize)
						currentMember->size = cclass->maxSize * (currentMember->arrayDim <= 0 ? 1 : currentMember->arrayDim);
				}
			}
		}
	};


	auto genUnknownMember = [&](int from, int to, int special)
	{
		EngineStructs::Member unknown;
		unknown.missed = true;
		unknown.size = to - from;
		char name[30];
		sprintf_s(name, "UnknownData%02d_%d[0x%X]", eStruct.unknownCount++, special, unknown.size);
		unknown.name = std::string(name);
		unknown.type = { false, PropertyType::BoolProperty, TYPE_UCHAR };
		unknown.offset = from;
		eStruct.undefinedMembers.push_back(unknown);
		eStruct.cookedMembers.push_back(std::pair(false, eStruct.undefinedMembers.size() - 1));
	};

	//end bit exclusive
	auto genUnknownBits = [&](int startOffset, int endOffset, int startBit, int endBit)
	{
		//weird
		if (endOffset < startOffset)
			return;

		//weird aswell
		if (endOffset == startOffset && startBit >= endBit)
			return;

		//are we here many offsets apart??
		//0x5:3
		//0x7:1
		//->
		//0x5:3
		//0x6 unknownmember[0x1]
		//0x7:0 unk (handled by while)
		//0x7:1
		if (endOffset - startOffset > 1)
		{
			//fill that with a unknownmember instead of bits
			//if the start bit is 0, which indicates the last defined bit was a 8th bit,
			//we dont have to increase the startOffset as it would directly fit, however of the
			//startbit is something else, we have to increase the start offset
			//----case 1 ----
			// 0x10: 00000000 <- last defined bit was 8th, function gets called with 0x11 startOffset and startbit 0 
			// 0x11: unknown
			//----case 2 ----
			// 0x10: 0000---- <- last defined bit was 4th, function gets called with 0x10 startoffset and startbit 5
			// 0x11: unknown
			genUnknownMember(startBit == 0 ? startOffset : startOffset + 1, endOffset, 3);
			//check if the end is < 0, then we can just stop
			if (endBit == 0)
				return;
			//adjust, now we just gotta fill the bits until endbit
			startOffset = endOffset;
			startBit = 0;
		}

		while (true)
		{
			if (startOffset == endOffset && startBit == endBit)
			{
				break;
			}
			EngineStructs::Member unknown;
			unknown.missed = true;
			char name[30];
			sprintf_s(name, "UnknownBit%02d", eStruct.unknownCount++);
			unknown.name = std::string(name);
			unknown.offset = startOffset;
			unknown.size = 1;
			unknown.type = { false, PropertyType::BoolProperty, TYPE_UCHAR };
			unknown.isBit = true;
			unknown.bitOffset = startBit++;
			if (startBit >= 8) //should actually just be == 8 otherwise its super weird
			{
				startBit = startBit % 8;
				startOffset++;
			}
			eStruct.undefinedMembers.push_back(unknown);
			eStruct.cookedMembers.push_back(std::pair(false, eStruct.undefinedMembers.size() - 1));
		}
	};

	if (eStruct.size - eStruct.getInheritedSize() == 0)
		return;

	if (eStruct.definedMembers.size() == 0)
	{
		if (eStruct.inherited)
		{
			const auto& inherStruct = eStruct.supers[0];
			if (eStruct.maxSize - inherStruct->maxSize > 0)
			{
				genUnknownMember(inherStruct->maxSize, eStruct.maxSize, 1);
			}
		}
		else if (eStruct.maxSize > 0)
		{
			genUnknownMember(0, eStruct.maxSize, 2);
		}
		return;
	}

	if (eStruct.inherited)
	{
		const auto& inherStruct = eStruct.supers[0];
		if (inherStruct->maxSize < eStruct.definedMembers[0].offset)
		{
			// If this happens for CoreUObject, then we are about to introduce excessive padding of an unknown member and not take in to account the parent's members, resulting in members being offset from reality
			if (inherStruct->maxSize == 0 && inherStruct->fullName == "/Script/CoreUObject.Object") {
				windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_ERROR, "ENGINECORE", "%s maxSize is zero! SDK for %s may not work correctly!", inherStruct->fullName.c_str(), eStruct.fullName.c_str());
				printf("%s maxSize is zero! SDK for %s may not work correctly!\n", inherStruct->fullName.c_str(), eStruct.fullName.c_str());
			}
			genUnknownMember(inherStruct->maxSize, eStruct.definedMembers[0].offset, 8);
		}
	}
	else if(!eStruct.definedMembers.empty())
	{
		const auto& firstMember = eStruct.definedMembers[0];
		if(firstMember.offset != 0)
			genUnknownMember(0, eStruct.definedMembers[0].offset, 7);
	}



	//we are hoping (very hard) that definedmembers array is 1. sorted and 2. checked for collisions
	for (int i = 0; i < eStruct.definedMembers.size() - 1; i++)
	{
		auto& currentMember = eStruct.definedMembers[i];
		const auto& nextMember = eStruct.definedMembers[i + 1];

		//bit shit
		if (currentMember.isBit)
		{
			eStruct.cookedMembers.push_back(std::pair(true, i));
			if (nextMember.isBit)
			{
				//not directly next to it?
				//0x7:3
				//0x7:4
				if (currentMember.offset == nextMember.offset && nextMember.bitOffset - currentMember.bitOffset > 1)
				{
					genUnknownBits(currentMember.offset, nextMember.offset, currentMember.bitOffset + 1, nextMember.bitOffset);
					continue;
				}
				//offset diff?
				//0x6:2
				//0x7:4
				if (nextMember.offset > currentMember.offset)
				{
					int startBitOffset = currentMember.bitOffset + 1;
					int startOffset = currentMember.offset;

					//we cant use bitoffset + 1 here because 8 is a invalid state, so we fixup that the gen starts at off+1 and bit 0
					if (currentMember.bitOffset == 7)
					{
						startBitOffset = 0;
						startOffset++;
					}
					genUnknownBits(startOffset, nextMember.offset, startBitOffset, nextMember.bitOffset);
					continue;
				}
			}
			//is the next member offset not directly after it?
			//0x5:3
			//0x8
			//->
			//0x5:3
			//0x6 unk[0x2]
			//0x8 (handled by next iter)
			if (nextMember.offset - currentMember.offset > 1)
			{
				genUnknownMember(currentMember.offset + 1, nextMember.offset, 5);
			}
			continue;
		}
		eStruct.cookedMembers.push_back(std::pair(true, i));
		//0x2 [0x4]
		//0x7 [0x2]
		//->
		//0x2 [0x4]
		//0x6 unk[0x1]
		//0x7 [0x2]


		checkRealMemberSize(&currentMember);


		if (nextMember.offset - (currentMember.offset + currentMember.size) > 0)
		{
			genUnknownMember(currentMember.offset + currentMember.size, nextMember.offset, 6);
		}

		//fixup any bits
		if (nextMember.isBit && nextMember.bitOffset > 0)
		{
			genUnknownBits(nextMember.offset, nextMember.offset, 0, nextMember.bitOffset);
		}
	}
	//add the last member
	eStruct.cookedMembers.push_back(std::pair(true, eStruct.definedMembers.size() - 1));
	auto last = eStruct.getMemberForIndex(eStruct.cookedMembers.size() - 1);
	checkRealMemberSize(last);
	if (last->offset + last->size < eStruct.maxSize)
		genUnknownMember(last->offset + last->size, eStruct.maxSize, 7);
}


EngineCore::EngineCore()
{
	static bool loaded = false;

	bSuccess = false;
	if (!loaded)
	{
		windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_ONLY_LOG, "ENGINECORE", "Loading core...");

		offsets = setOffsets();


		gNames = getOffsetAddress(getOffsetForName("OFFSET_GNAMES"));
		windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_INFO, "ENGINECORE", "GNames -> 0x%p", gNames);
		if (!gNames)
		{
			windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_ERROR, "ENGINECORE", "GNames offset not found!");
			return;
		}

#if UE_VERSION < UE_4_24

		//in < 4.25 we have to get the heap pointer
		gNames = Memory::read<uint64_t>(gNames);
		windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_INFO, "ENGINECORE", "GNames -> 0x%p", gNames);
		if (!gNames)
		{
			windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_ERROR, "ENGINECORE", "GNames offset seems zero!");
			return;
		}
#endif



		loaded = true;
	}

	bSuccess = true;

}

bool EngineCore::initSuccess()
{
	return bSuccess;
}

void EngineCore::cacheFNames(int64_t & finishedNames, int64_t & totalNames, CopyStatus & status)
{
	windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_ONLY_LOG, "ENGINECORE", "Caching FNames...");
	status = CS_busy;
	totalNames = ObjectsManager::gUObjectManager.UObjectArray.NumElements;
	finishedNames = 0;
	bool bIsFirstValidObject = true;

	for (; finishedNames < ObjectsManager::gUObjectManager.UObjectArray.NumElements; finishedNames++)
	{
		const auto object = ObjectsManager::getUObjectByIndex<UObject>(finishedNames);
		if (!object)
			continue;

		//caches already if not cached, we dont have to use the result
		auto res = object->getName();

#if BREAK_IF_INVALID_NAME
		if (bIsFirstValidObject && res != "/Script/CoreUObject")
		{
			windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_ERROR, "ENGINECORE",
				"ERROR: The first object name should be always /Script/CoreUObject! Instead got \"%s\".This is most likely the result of a invalid FName offset or no decryption!", res.c_str());
			status = CS_error;
			return;
		}
		bIsFirstValidObject = false;

#endif
	}
	status = CS_success;
	windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_ONLY_LOG, "ENGINECORE", "Cached all FNames!");
}

void EngineCore::generatePackages(int64_t& finishedPackages, int64_t& totalPackages, CopyStatus& status)
{
	windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_INFO, "ENGINECORE", "Caching all Packages...");
	status = CS_busy;

	//we already done?
	if (packages.size() > 0)
	{
		status = CS_success;
		totalPackages = packages.size();
		finishedPackages = packages.size();
		windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_ONLY_LOG, "ENGINECORE", "Packages already got cached!");
		return;
	}
	std::unordered_map<std::string, std::vector<UObject*>> upackages;
	//we first use the counter to iterate over all elements
	totalPackages = ObjectsManager::gUObjectManager.UObjectArray.NumElements;
	finishedPackages = 0;

	windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_ONLY_LOG, "ENGINECORE", "reading overriding structs....");
	overrideStructs();
	windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_ONLY_LOG, "ENGINECORE", "adding custom structs....");
	addStructs();
	addEnums();
	windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_ONLY_LOG, "ENGINECORE", "adding overriding unknown members....");
	overrideUnknownMembers();

	int numUStructsFound = 0;
	int numEnumsFound = 0;
	for (; finishedPackages < ObjectsManager::gUObjectManager.UObjectArray.NumElements; finishedPackages++)
	{
		auto object = ObjectsManager::getUObjectByIndex<UObject>(finishedPackages);

		//instantly go if any operation was not successful!
		if (ObjectsManager::CRITICAL_STOP_CALLED())
			return;

		//is it even valid? Some indexes arent
		if (!object)
			continue;

		bool isUStruct = false, isEnum = false;
		if (object->IsA<UStruct>()) {
			numUStructsFound++;
			isUStruct = true;
		}

		if (object->IsA<UEnum>()) {
			numEnumsFound++;
			isEnum = true;
		}

		if (!isUStruct && !isEnum)
			continue;

		upackages[object->getSecondPackageName()].push_back(object);
	}
	if (numUStructsFound == 0) {
		windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_WARNING, "ENGINECORE", "WARN: No UStruct objects found");
	}
	if (numEnumsFound == 0) {
		windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_WARNING, "ENGINECORE", "WARN: No Enum objects found");
	}

	//reset the counter to 0 as we are using it again but this time really for packages
	finishedPackages = 0;
	totalPackages = upackages.size();
	windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_INFO, "ENGINECORE", "Total packages: %d", totalPackages);



	EngineStructs::Package basicType;
	basicType.index = 0;
	basicType.packageName = "BasicType"; //dont rename!!
	for (auto& struc : customStructs)
	{
		auto& dataVector = struc.isClass ? basicType.classes : basicType.structs;
		dataVector.push_back(struc);
	}
	for (auto& struc : customEnums)
		basicType.enums.push_back(struc);

	packages.push_back(basicType);

	std::unordered_map<std::string, std::string> usedNames;

	auto checkForDuplicateNames = [&usedNames](EngineStructs::Package package) {
		for (auto& enu : package.enums) {
			if (usedNames.contains(enu.cppName)) {
				windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_WARNING, "CORE", "Enum redefinitio in package %s! %s has already been defined in package %s", package.packageName.c_str(), enu.cppName.c_str(), usedNames[enu.cppName].c_str());
				printf("Enum redefinition in package %s! %s has already been defined in package %s\n", package.packageName.c_str(), enu.cppName.c_str(), usedNames[enu.cppName].c_str());
			}
			usedNames.insert({ enu.cppName, package.packageName });
		}
		for (auto& struc : package.combinedStructsAndClasses) {
			if (usedNames.contains(struc->cppName)) {
				windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_WARNING, "CORE", "%s redefinition in package %s! %s has already been defined in package %s", struc->isClass ? "Class" : "Struct", struc->cppName.c_str(), usedNames[struc->cppName].c_str());
				printf("%s redefinition in package %s! %s has already been defined in package %s\n", struc->isClass ? "Class" : "Struct", package.packageName.c_str(), struc->cppName.c_str(), usedNames[struc->cppName].c_str());
			}
			usedNames.insert({ struc->cppName, package.packageName });
		}
	};

	//package 0 is reserved for our special defined structs
	for (auto& package : upackages)
	{
		EngineStructs::Package ePackage;
		ePackage.packageName = package.first;


		for (const auto& object : package.second)
		{
			const bool isClass = object->IsA<UClass>();
			if (ObjectsManager::CRITICAL_STOP_CALLED())
				return;
			if (isClass || object->IsA<UScriptStruct>())
			{
				auto& dataVector = isClass ? ePackage.classes : ePackage.structs;
				const auto naming = isClass ? "Class" : "Struct";



				//is the struct predefined?
				if (overridingStructs.contains(object->getFullName()))
				{
					windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_ONLY_LOG, "CORE", "%s %s is predefined!", naming, object->getCName().c_str());
					auto& struc = overridingStructs[object->getFullName()];
					//last check, does the cpp name match?
					if (struc.cppName == object->getCName())
					{
						struc.memoryAddress = reinterpret_cast<uintptr_t>(object->getOwnPointer());

						dataVector.push_back(struc);

						auto& generatedStruc = dataVector.back();
						generatedStruc.isClass = isClass;

						generateFunctions(object->castTo<UStruct>(), generatedStruc.functions);

						windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_INFO, "CORE", "Total member count: %d | Function count: %d", generatedStruc.cookedMembers.size(), generatedStruc.functions.size());

						continue;
					}
				}

				if (ObjectsManager::CRITICAL_STOP_CALLED())
					return;

				windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_INFO, "CORE",
					"Generating %s %s::%s", naming, ePackage.packageName.c_str(), object->getCName().c_str());
				printf("Generating %s %s::%s\n", naming, ePackage.packageName.c_str(), object->getCName().c_str());


				const auto sObject = object->castTo<UStruct>();

				if (!generateStructOrClass(sObject, dataVector))
					continue;

				auto& generatedStruc = dataVector.back();
				generatedStruc.isClass = isClass;

				windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_INFO, "CORE", "Total member count: %d | Function count: %d", generatedStruc.definedMembers.size(), generatedStruc.functions.size());

			}
			else if (object->IsA<UEnum>())
			{
				windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_INFO, "CORE", "Generating Enum %s", object->getCName().c_str());
				const auto eObject = object->castTo<UEnum>();
				if (!generateEnum(eObject, ePackage.enums))
					continue;
			}
		}

		checkForDuplicateNames(ePackage);

		packages.push_back(ePackage);
		finishedPackages++;
	}


	std::ranges::sort(packages, EngineStructs::Package::packageCompare);

	//were done, now we do packageObjectInfos caching, we couldnt do before because pointers are all on stack data and not in the static package vec
	finishPackages();

	status = CS_success;
	windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_INFO, "ENGINECORE", "Done generating packages!");
}

std::vector<EngineStructs::Package>& EngineCore::getPackages()
{
	return packages;
}


const ObjectInfo* EngineCore::getInfoOfObject(const std::string & CName)
{
	//in functions we compare packageIndex and objectIndex anyways so the type doesnt matter
	if (!packageObjectInfos.contains(CName))
		return nullptr;

	return &packageObjectInfos[CName];
}


const std::vector<std::string>& EngineCore::getAllUnknownTypes()
{
	//already checked? Well then dont do it again
	if (unknownProperties.size() > 0)
		return unknownProperties;

	for (auto& pack : packages)
	{
		auto checkMembers = [&](const EngineStructs::Struct& struc) mutable
		{
			for (auto& member : struc.definedMembers)
			{
				if (!member.type.clickable || //not clickable? Skip
					packageObjectInfos.contains(member.type.name) || //packageObjectInfos contains the name? Then its defined
					std::ranges::find(unknownProperties, member.type.name) != unknownProperties.end()) //is it already in the vector? Skip
					continue;

				unknownProperties.push_back(member.type.name);
			}
		};

		for (auto& struc : pack.structs)
			checkMembers(struc);
		for (auto& struc : pack.classes)
			checkMembers(struc);
	}
	return unknownProperties;
}

void EngineCore::overrideStruct(EngineStructs::Struct & eStruct)
{
	if (overridingStructs.contains(eStruct.fullName))
		return;

	overridingStructs.insert(std::pair(eStruct.fullName, eStruct));
}

void EngineCore::createStruct(const EngineStructs::Struct & eStruct)
{
	if (std::ranges::find(customStructs, eStruct) != customStructs.end())
		return;

	customStructs.push_back(eStruct);
}

void EngineCore::createEnum(const EngineStructs::Enum& eEnum)
{
	if (std::ranges::find(customEnums, eEnum) != customEnums.end())
		return;

	customEnums.push_back(eEnum);
}

void EngineCore::overrideStructMembers(const EngineStructs::Struct & eStruct)
{
	if (overridingStructMembers.contains(eStruct.fullName))
		return;

	overridingStructMembers.insert(std::pair(eStruct.fullName, eStruct));
}

void EngineCore::finishPackages()
{
	std::unordered_map<std::string, EngineStructs::Enum*> enumLookupTable;
	std::unordered_map<std::string, int> enumMap = {};
	std::unordered_set<std::string> duplicatedClassNames{};
	int duplicatedNames = 0;
	//were done, now we do packageObjectInfos caching, we couldnt do before because pointers are all on stack data and not in the static package vec
	for (int i = 0; i < packages.size(); i++)
	{
		auto& package = packages[i];
		package.index = i;

		auto fillMissingDataForStructs = [&](std::vector<EngineStructs::Struct>& structs)
		{
			for (int j = 0; j < structs.size(); j++)
			{
				auto& struc = structs[j];
				struc.owningPackage = &package;
				struc.owningVectorIndex = j;

				const auto OI_type = struc.isClass ? ObjectInfo::OI_Class : ObjectInfo::OI_Struct;
				if (packageObjectInfos.contains(struc.cppName))
				{
					windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_WARNING, "ENGINECORE", "Duplicate name found: %s", struc.cppName.c_str());

					if (!duplicatedClassNames.contains(struc.cppName))
						duplicatedClassNames.insert(struc.cppName);
					struc.cppName += "dup_" + std::to_string(duplicatedNames++);
					
				}
				packageObjectInfos.insert(std::pair(struc.cppName, ObjectInfo(true, OI_type, &struc)));
				package.combinedStructsAndClasses.push_back(&struc);

				for (int k = 0; k < struc.functions.size(); k++)
				{
					auto& func = struc.functions[k];
					func.owningVectorIndex = k;
					func.owningStruct = &struc;
					package.functions.push_back(&func);

					packageObjectInfos.insert(std::pair(func.cppName, ObjectInfo(true, ObjectInfo::OI_Function, &func)));

				}

				//empty structs have a size of 1
				if(!struc.isClass && struc.maxSize == 0)
				{
					struc.size = 1;
					struc.maxSize = 1;
				}
			}
		};

		fillMissingDataForStructs(package.classes);
		fillMissingDataForStructs(package.structs);

		for (int j = 0; j < package.enums.size(); j++)
		{
			auto& enu = package.enums[j];
			enu.owningPackage = &package;
			enu.owningVectorIndex = j;
			packageObjectInfos.insert(std::pair(enu.cppName, ObjectInfo(true, ObjectInfo::OI_Enum, &enu)));
		}
	}

	//we have to loop again for dependency tracking and supers
	for (int i = 0; i < packages.size(); i++)
	{
		auto& package = packages[i];

		for (const auto& struc : package.combinedStructsAndClasses)
		{
			for (const auto& var : struc->definedMembers)
			{
				if (var.type.propertyType == PropertyType::EnumProperty && !enumMap.contains(var.type.name))
				{
					enumMap.insert(std::pair<std::string, int>(var.type.name, var.arrayDim > 0 ? var.size / var.arrayDim : var.size));
				}
			}

			for (auto& name : struc->superNames)
			{
				const auto info = getInfoOfObject(name);
				if (!info || !info->valid || (info->type != ObjectInfo::OI_Class && info->type != ObjectInfo::OI_Struct))
					continue;
				//get the super struct
				auto superStruc = static_cast<EngineStructs::Struct*>(info->target);
				//add the super struct as a super
				struc->supers.push_back(superStruc);
				//if they arent in the same package, add the supers package as dependency
				if (superStruc->owningPackage->index != package.index)
					package.dependencyPackages.insert(superStruc->owningPackage);
				//add the current struct to the list of super of others of the super
				superStruc->superOfOthers.push_back(struc);
				//now if our current struct has defined members, we check the size of the super and possibly reduce the maxSize
				//because of trailing and padding, we choose the lowest member
				if (struc->definedMembers.size() > 0)
				{
					const auto& firstMember = struc->definedMembers[0];
					if (firstMember.offset < superStruc->maxSize)
					{
						superStruc->maxSize = firstMember.offset;
					}
				}
			}

			//this check works only with broken structs
			if(struc->supers.size() > 0)
			{
				//is the super max size is greater than the max size of the struct itself we have some weird cringe struc
				auto super = struc->supers[0];
				if(super->maxSize > struc->maxSize)
				{
					struc->maxSize = super->maxSize;
					struc->size = super->maxSize;
				}
			}

			for (auto& var : struc->definedMembers)
			{
				if (!var.type.clickable)
					continue;
				const auto info = getInfoOfObject(var.type.name);
				if (!info || !info->valid)
					continue;

				//if the type is a type where dumplicate classes exist, we have to erase it
				//theres no way to know which one of the dup classes it refers to
				//or maybe there is a way? maybe in the future with pointers or so....
				if(duplicatedClassNames.contains(var.type.name))
				{
					var.type.clickable = false;
					var.type.propertyType = PropertyType::Int8Property;
					var.arrayDim = var.size;
					var.name += "_unkBecDupClass_" + var.type.name;
					var.type.name = TYPE_UCHAR;
				}

				var.type.info = info;

				for (auto& subtype : var.type.subTypes)
				{
					if (!subtype.clickable)
						continue;
					const auto subInfo = getInfoOfObject(subtype.name);
					if (!subInfo || !subInfo->valid)
						continue;

					subtype.info = subInfo;

					if (subtype.propertyType != PropertyType::ObjectProperty && subtype.propertyType != PropertyType::ClassProperty)
					{
						const auto targetPack = subInfo->type == ObjectInfo::OI_Enum ? static_cast<EngineStructs::Enum*>(subInfo->target)->owningPackage : static_cast<EngineStructs::Struct*>(subInfo->target)->owningPackage;
						if (targetPack->index != package.index)
							package.dependencyPackages.insert(targetPack);
					}
				}

				const auto targetPack = info->type == ObjectInfo::OI_Enum ? static_cast<EngineStructs::Enum*>(info->target)->owningPackage : static_cast<EngineStructs::Struct*>(info->target)->owningPackage;
				if (targetPack->index != package.index)
					package.dependencyPackages.insert(targetPack);

			}
		}

		for (const auto& func : package.functions)
		{
			auto& ret = func->returnType;
			auto addInfoPtr = [&](fieldType& type)
			{
				if (type.clickable)
				{
					const auto info = getInfoOfObject(type.name);
					if (info && info->valid)
					{
						type.info = info;

						const auto targetPack = info->type == ObjectInfo::OI_Enum ? static_cast<EngineStructs::Enum*>(info->target)->owningPackage : static_cast<EngineStructs::Struct*>(info->target)->owningPackage;
						if (targetPack->index != package.index)
							package.dependencyPackages.insert(targetPack);
					}
				}
			};
			addInfoPtr(ret);

			for (auto& param : func->params)
			{
				auto& type = std::get<0>(param);
				addInfoPtr(type);
			}
		}

		for (int j = 0; j < package.enums.size(); j++)
		{
			enumLookupTable.insert(std::pair<std::string, EngineStructs::Enum*>(generateValidVarName(package.enums[j].cppName), &package.enums[j]));
		}

	}

	// Correct Enum types based on actual member data (they were previously guessed based on max value).
	// We iterate over the enum map to assert we cover all enums that have overrides
	for (auto iter = enumMap.begin(); iter != enumMap.end(); iter++)
	{
		if (!enumLookupTable.contains(iter->first) && !overridingStructs.contains(iter->first))
		{
			// predefined Enums like EOjbectFlags will hit this condition
			continue;
		}
		enumLookupTable[iter->first]->type = getEnumTypeFromSize(iter->second);
		enumLookupTable[iter->first]->size = iter->second;
	}

	for (auto& package : packages)
	{
		for (auto& struc : package.structs)
			cookMemberArray(struc);
		for (auto& clas : package.classes)
			cookMemberArray(clas);
	}

}

void EngineCore::runtimeOverrideStructMembers(EngineStructs::Struct * eStruct, const std::vector<EngineStructs::Member>&members)
{
	if (eStruct == nullptr)
		return;
	for (const auto& member : members)
	{
		RUNAddMemberToMemberArray(*eStruct, member);
	}
	cookMemberArray(*eStruct);
}

void EngineCore::saveToDisk(int& progressDone, int& totalProgress)
{
	totalProgress = 1 + FNameCache.size() + packageObjectInfos.size() +
		overridingStructs.size() + packages.size() + unknownProperties.size() + customStructs.size() + offsets.size() + 5000;
	progressDone = 0;
	windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_INFO, "ENGINECORE", "Saving to disk...");
	nlohmann::json UEDProject;


	UEDProject["EngineSettings"] = EngineSettings::toJson();
	progressDone++;

	nlohmann::json unordered_maps;

	nlohmann::json jFNameCache;
	for (const auto& entry : FNameCache)
		jFNameCache[std::to_string(entry.first)] = entry.second;
	unordered_maps["FNameCache"] = jFNameCache;
	progressDone += FNameCache.size();


	nlohmann::json jOverridingStructs;
	for (const auto& entry : overridingStructs)
		jOverridingStructs[entry.first] = entry.second.toJson();
	unordered_maps["OverridingStructs"] = jOverridingStructs;
	progressDone += overridingStructs.size();

	UEDProject["unordered_maps"] = unordered_maps;

	nlohmann::json vectors;

	nlohmann::json jPackages;
	for (const auto& package : packages)
		jPackages.push_back(package.toJson());
	vectors["Packages"] = jPackages;
	progressDone += packages.size();

	nlohmann::json jUnknownProperties = unknownProperties;
	vectors["UnknownProperties"] = jUnknownProperties;
	progressDone += unknownProperties.size();

	// Create JSON for customStructs vector
	nlohmann::json jCustomStructs;
	for (const auto& structObj : customStructs)
		jCustomStructs.push_back(structObj.toJson());
	vectors["CustomStructs"] = jCustomStructs;
	progressDone += customStructs.size();

	// Create JSON for offsets vector
	nlohmann::json jOffsets;
	for (const auto& offset : offsets)
		jOffsets.push_back(offset.toJson());
	vectors["Offsets"] = jOffsets;
	progressDone += offsets.size();

	UEDProject["vectors"] = vectors;

	UEDProject["OpenTabs"] = windows::PackageViewerWindow::getTabsToJson();

	auto dump = UEDProject.dump(-1, ' ', false, nlohmann::detail::error_handler_t::replace);

	size_t paddingBytes = 16 - (dump.length() % 16);
	auto totalBytes = dump.length() + paddingBytes;

	unsigned char* strBytes = static_cast<unsigned char*>(calloc(1, totalBytes));
	std::memcpy(strBytes, dump.data(), dump.length());

	//super secret
	const char* key = "UEDumper secret!";

	AES aes(AESKeyLength::AES_128);
	auto c = aes.EncryptECB(strBytes, totalBytes, reinterpret_cast<const unsigned char*>(key));


	std::ofstream file(EngineSettings::getWorkingDirectory() / "SaveState.uedproj", std::ios::binary);

	file.write(reinterpret_cast<const char*>(c), totalBytes);
	file.close();

	free(strBytes);
	delete[] c;

	windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_INFO, "ENGINECORE", "Saved!");
	progressDone = totalProgress;
}

bool EngineCore::loadProject(const std::string & filepath, int& progressDone, int& totalProgress)
{
	progressDone = 0;
	totalProgress = 11;
	std::ifstream file(filepath, std::ios::binary | std::ios::ate);
	if (!file)
	{
		windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_ERROR, "ENGINECORE", "Error opening file!");
		return false;
	}

	const std::size_t fileSize = file.tellg();
	file.seekg(0, std::ios::beg);

	if (fileSize < 100)
	{
		windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_ERROR, "ENGINECORE", "File invalid!");
		return false;
	}

	unsigned char* buffer = static_cast<unsigned char*>(calloc(1, fileSize));
	if (!file.read(reinterpret_cast<char*>(buffer), fileSize))
	{
		windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_ERROR, "ENGINECORE", "Failed to read file!");
		free(buffer);
		return false;
	}

	progressDone++;
	//seems like you found the easteregg that i encrypt the project with aes
	const char* key = "UEDumper secret!";

	AES aes(AESKeyLength::AES_128);
	auto c = aes.DecryptECB(buffer, fileSize, reinterpret_cast<const unsigned char*>(key));

	free(buffer);

	std::string cmp = "{\"EngineSettings\"";

	if (std::memcmp(c, cmp.c_str(), cmp.length()) != 0)
	{
		windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_ERROR, "ENGINECORE", "Wrong decryption key!");
		delete[] c;
		return false;
	}

	const nlohmann::json UEDProject = nlohmann::json::parse(c);



	delete[] c;



	//now set all enginesettings settings
	const nlohmann::json engineSettings = UEDProject["EngineSettings"];

	if (!EngineSettings::loadJson(engineSettings))
		return false;

	progressDone++;

	const nlohmann::json unordered_maps = UEDProject["unordered_maps"];

	if (!unordered_maps.contains("FNameCache") ||
		!unordered_maps.contains("OverridingStructs"))
	{
		windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_ERROR, "ENGINECORE", "Project corrupted! (-4)");
		return false;
	}


	nlohmann::json jFNameCache = unordered_maps["FNameCache"];
	for (auto it = jFNameCache.begin(); it != jFNameCache.end(); ++it)
	{
		FNameCache.insert(std::pair(std::stoi(it.key()), it.value()));
	}


	progressDone++;

	nlohmann::json jOverridingStructs = unordered_maps["OverridingStructs"];
	for (auto it = jOverridingStructs.begin(); it != jOverridingStructs.end(); ++it)
	{
		overridingStructs.insert(std::pair(it.key(), EngineStructs::Struct::fromJson(it.value())));
	}

	progressDone++;

	const nlohmann::json vectors = UEDProject["vectors"];

	if (!vectors.contains("Packages") ||
		!vectors.contains("CustomStructs") ||
		!vectors.contains("Offsets"))
	{
		windows::LogWindow::Log(windows::LogWindow::logLevels::LOGLEVEL_ERROR, "ENGINECORE", "Project corrupted! (-5)");
		return false;
	}

	nlohmann::json jPackages = vectors["Packages"];
	for (const nlohmann::json& package : jPackages)
		packages.push_back(EngineStructs::Package::fromJson(package));

	finishPackages();

	progressDone++;

	unknownProperties = vectors["UnknownProperties"];

	progressDone++;

	nlohmann::json jCustomStructs = vectors["CustomStructs"];

	for (const nlohmann::json& customStruct : jCustomStructs)
		customStructs.push_back(EngineStructs::Struct::fromJson(customStruct));

	progressDone++;

	nlohmann::json jOffsets = vectors["Offsets"];
	for (const nlohmann::json& offset : jOffsets)
		offsets.push_back(Offset::fromJson(offset));

	progressDone++;

	windows::PackageViewerWindow::loadTabsFromJson(UEDProject["OpenTabs"]);
	progressDone++;

	EngineSettings::setLiveEditor(false);

	progressDone = totalProgress;


	return true;
}


void EngineCore::generateStructDefinitionsFile()
{
	std::ofstream file(EngineSettings::getWorkingDirectory() / "StructDefinitions.txt");
	file << "/// All changes made to structs are dumped here.\n\n" << std::endl;


	auto printToFile = [&](std::unordered_map<std::string, EngineStructs::Struct>& map) mutable
	{
		auto boolToSt = [](bool b)
		{
			return b ? "true" : "false";
		};

		for (auto& val : map | std::views::values)
		{
			std::string spacing = "    ";
			std::string objectName = "s" + val.cppName;
			file << spacing << "EngineStructs::Struct " << objectName << ";" << std::endl;
			file << spacing << objectName << ".fullName = " << "\"" << val.fullName << "\";" << std::endl;
			file << spacing << objectName << ".cppName = " << "\"" << val.cppName << "\";" << std::endl;
			file << spacing << objectName << ".size = " << val.size << ";" << std::endl;
			file << spacing << objectName << ".inherited = " << boolToSt(val.inherited) << ";" << std::endl;
			file << spacing << objectName << ".isClass = " << boolToSt(val.isClass) << ";" << std::endl;
			file << spacing << objectName << ".members = std::vector<EngineStructs::Member> {" << std::endl;
			for (int i = 0; i < val.cookedMembers.size(); i++)
			{
				auto printFieldType = [&](const fieldType& type) mutable
				{
					file << "{" << boolToSt(type.clickable) << ", " << getStringFromPropertyType(type.propertyType)
						<< ", \"" << type.name << "\"";
				};

				const auto member = val.getMemberForIndex(i);
				file << spacing << spacing << "{";
				printFieldType(member->type);

				if (member->type.subTypes.size() > 0)
				{
					file << ", std::vector<fieldType>{";
					for (int j = 0; j < member->type.subTypes.size(); j++)
					{
						printFieldType(member->type.subTypes[j]);
						file << "}";
						if (j < member->type.subTypes.size() - 1)
							file << ",";
					}
					file << "}";
				}
				file << "}, \"" << member->name << "\", " << member->offset << ", " << member->size << ", " << boolToSt(member->missed);
				if (member->isBit)
				{

					file << ", " << boolToSt(member->isBit) << ", " << (member->bitOffset >= 99 ? member->bitOffset - 99 : member->bitOffset) << ", " << boolToSt(member->userEdited);
				}
				file << "}," << std::endl;
			}
			file << spacing << "};" << std::endl;
			file << spacing << "EngineCore::overrideStructMembers(" << objectName << ");\n\n" << std::endl;

		}
	};

	file << "/// overrideStructs\n" << std::endl;
	printToFile(overridingStructs);

	//customstructs is not a unordered map, its a vector. so we create a fake map to use the function above
	std::unordered_map<std::string, EngineStructs::Struct> customStructsMap;
	for (const auto& struc : customStructs)
		customStructsMap.insert(std::pair(struc.fullName, struc));
	file << "/// customStructs\n" << std::endl;
	printToFile(customStructsMap);
	file << "/// overridingStructMembers\n" << std::endl;
	printToFile(overridingStructMembers);
	file.close();

}
