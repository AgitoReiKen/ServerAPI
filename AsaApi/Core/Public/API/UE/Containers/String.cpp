// Copyright Epic Games, Inc. All Rights Reserved.

#include "Containers/Array.h"
#include "Containers/StringConv.h"
#include "Containers/UnrealString.h"
#include "CoreGlobals.h"
#include "CoreTypes.h"
#include "HAL/PlatformString.h"
#include "HAL/UnrealMemory.h"
//#include "Logging/LogCategory.h"
//#include "Logging/LogMacros.h"
#include "Math/NumericLimits.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/AssertionMacros.h"
#include "Misc/ByteSwap.h"
#include "Misc/CString.h"
#include "Misc/Char.h"
#include "Misc/VarArgs.h"
#include "Serialization/Archive.h"
#include "String/HexToBytes.h"
#include "Templates/MemoryOps.h"
#include "Templates/RemoveReference.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/NameTypes.h"
#include "API/Fields.h"

/* FString implementation
 *****************************************************************************/

namespace UE::Core::String::Private
{
	struct FCompareCharsCaseSensitive
	{
		static FORCEINLINE bool Compare(TCHAR Lhs, TCHAR Rhs)
		{
			return Lhs == Rhs;
		}
	};

	struct FCompareCharsCaseInsensitive
	{
		static FORCEINLINE bool Compare(TCHAR Lhs, TCHAR Rhs)
		{
			return FChar::ToLower(Lhs) == FChar::ToLower(Rhs);
		}
	};

	template <typename CompareType>
	bool MatchesWildcardRecursive(const TCHAR* Target, int32 TargetLength, const TCHAR* Wildcard, int32 WildcardLength)
	{
		// Skip over common initial non-wildcard-char sequence of Target and Wildcard
		for (;;)
		{
			if (WildcardLength == 0)
			{
				return TargetLength == 0;
			}

			TCHAR WCh = *Wildcard;
			if (WCh == TEXT('*') || WCh == TEXT('?'))
			{
				break;
			}

			if (!CompareType::Compare(*Target, WCh))
			{
				return false;
			}

			++Target;
			++Wildcard;
			--TargetLength;
			--WildcardLength;
		}

		// Test for common suffix
		const TCHAR* TPtr = Target   + TargetLength;
		const TCHAR* WPtr = Wildcard + WildcardLength;
		for (;;)
		{
			--TPtr;
			--WPtr;

			TCHAR WCh = *WPtr;
			if (WCh == TEXT('*') || WCh == TEXT('?'))
			{
				break;
			}

			if (!CompareType::Compare(*TPtr, WCh))
			{
				return false;
			}

			--TargetLength;
			--WildcardLength;

			if (TargetLength == 0)
			{
				break;
			}
		}

		// Match * against anything and ? against single (and zero?) chars
		TCHAR FirstWild = *Wildcard;
		if (WildcardLength == 1 && (FirstWild == TEXT('*') || TargetLength < 2))
		{
			return true;
		}
		++Wildcard;
		--WildcardLength;

		// This routine is very slow, though it does ok with one wildcard
		int32 MaxNum = TargetLength;
		if (FirstWild == TEXT('?') && MaxNum > 1)
		{
			MaxNum = 1;
		}

		for (int32 Index = 0; Index <= MaxNum; ++Index)
		{
			if (MatchesWildcardRecursive<CompareType>(Target + Index, TargetLength - Index, Wildcard, WildcardLength))
			{
				return true;
			}
		}
		return false;
	}
}

template<typename CharType>
void AppendCharacters(TArray<TCHAR>& Out, const CharType* Str, int32 Count)
{
	check(Count >= 0);

	if (!Count)
	{
		return;
	}

	checkSlow(Str);

	int32 OldEnd = Out.Num();
	
	// Try to reserve enough space by guessing that the new length will be the same as the input length.
	// Include an extra gap for a null terminator if we don't already have a string allocated
	Out.AddUninitialized(Count + (OldEnd ? 0 : 1));
	OldEnd -= OldEnd ? 1 : 0;

	TCHAR* Dest = Out.GetData() + OldEnd;

	// Try copying characters to end of string, overwriting null terminator if we already have one
	TCHAR* NewEnd = FPlatformString::Convert(Dest, Count, Str, Count);
	if (!NewEnd)
	{
		// If that failed, it will have meant that conversion likely contained multi-code unit characters
		// and so the buffer wasn't long enough, so calculate it properly.
		int32 Length = FPlatformString::ConvertedLength<TCHAR>(Str, Count);

		// Add the extra bytes that we need
		Out.AddUninitialized(Length - Count);

		// Restablish destination pointer in case a realloc happened
		Dest = Out.GetData() + OldEnd;

		NewEnd = FPlatformString::Convert(Dest, Length, Str, Count);
		checkSlow(NewEnd);
	}
	else
	{
		int32 NewEndIndex = (int32)(NewEnd - Dest);
		if (NewEndIndex < Count)
		{
			Out.SetNumUninitialized(OldEnd + NewEndIndex + 1, /*bAllowShrinking=*/false);
		}
	}

	// (Re-)establish the null terminator
	*NewEnd = '\0';
}

namespace UE::String::Private
{

template<typename CharType>
FORCEINLINE void ConstructFromCString(/* Out */ TArray<TCHAR>& Data, const CharType* Src)
{
	if (Src && *Src)
	{
		int32 SrcLen  = TCString<CharType>::Strlen(Src) + 1;
		int32 DestLen = FPlatformString::ConvertedLength<TCHAR>(Src, SrcLen);
		Data.Reserve(DestLen);
		Data.AddUninitialized(DestLen);

		FPlatformString::Convert(Data.GetData(), DestLen, Src, SrcLen);
	}
}

template<typename CharType>
FORCEINLINE void ConstructWithLength(/* Out */ TArray<TCHAR>& Data, int32 InCount, const CharType* InSrc)
{
	if (InSrc)
	{
		int32 DestLen = FPlatformString::ConvertedLength<TCHAR>(InSrc, InCount);
		if (DestLen > 0 && *InSrc)
		{
			Data.Reserve(DestLen + 1);
			Data.AddUninitialized(DestLen + 1);

			FPlatformString::Convert(Data.GetData(), DestLen, InSrc, InCount);
			*(Data.GetData() + Data.Num() - 1) = TEXT('\0');
		}
	}
}

template<typename CharType>
FORCEINLINE void ConstructWithSlack(/* Out */ TArray<TCHAR>& Data, const CharType* Src, int32 ExtraSlack)
{
	if (Src && *Src)
	{
		int32 SrcLen = TCString<CharType>::Strlen(Src) + 1;
		int32 DestLen = FPlatformString::ConvertedLength<TCHAR>(Src, SrcLen);
		Data.Reserve(DestLen + ExtraSlack);
		Data.AddUninitialized(DestLen);

		FPlatformString::Convert(Data.GetData(), DestLen, Src, SrcLen);
	}
	else if (ExtraSlack > 0)
	{
		Data.Reserve(ExtraSlack + 1); 
	}
}

} // namespace UE::String::Private

/*FString::FString(const ANSICHAR* Str)								{ UE::String::Private::ConstructFromCString(Data, Str); }
FString::FString(const WIDECHAR* Str)								{ UE::String::Private::ConstructFromCString(Data, Str); }
FString::FString(const UTF8CHAR* Str)								{ UE::String::Private::ConstructFromCString(Data, Str); }
FString::FString(const UCS2CHAR* Str)								{ UE::String::Private::ConstructFromCString(Data, Str); }
FString::FString(int32 Len, const ANSICHAR* Str)					{ UE::String::Private::ConstructWithLength(Data, Len, Str); }
FString::FString(int32 Len, const WIDECHAR* Str)					{ UE::String::Private::ConstructWithLength(Data, Len, Str); }
FString::FString(int32 Len, const UTF8CHAR* Str)					{ UE::String::Private::ConstructWithLength(Data, Len, Str); }
FString::FString(int32 Len, const UCS2CHAR* Str)					{ UE::String::Private::ConstructWithLength(Data, Len, Str); }
FString::FString(const ANSICHAR* Str, int32 ExtraSlack)				{ UE::String::Private::ConstructWithSlack(Data, Str, ExtraSlack); }
FString::FString(const WIDECHAR* Str, int32 ExtraSlack)				{ UE::String::Private::ConstructWithSlack(Data, Str, ExtraSlack); }
FString::FString(const UTF8CHAR* Str, int32 ExtraSlack)				{ UE::String::Private::ConstructWithSlack(Data, Str, ExtraSlack); }
FString::FString(const UCS2CHAR* Str, int32 ExtraSlack)				{ UE::String::Private::ConstructWithSlack(Data, Str, ExtraSlack); }*/

FString::FString(const ANSICHAR* Str)								{ NativeCall<void, const ANSICHAR*>(this, "FString.FString(char*)", Str); }
FString::FString(const WIDECHAR* Str)								{ NativeCall<void, const WIDECHAR*>(this, "FString.FString(wchar_t*)", Str); }
FString::FString(const UTF8CHAR* Str)								{ UE::String::Private::ConstructFromCString(Data, Str); }
FString::FString(const UCS2CHAR* Str)								{ UE::String::Private::ConstructFromCString(Data, Str); }
FString::FString(int32 Len, const ANSICHAR* Str)					{ UE::String::Private::ConstructWithLength(Data, Len, Str); }
FString::FString(int32 Len, const WIDECHAR* Str)					{ UE::String::Private::ConstructWithLength(Data, Len, Str); }
FString::FString(int32 Len, const UTF8CHAR* Str)					{ UE::String::Private::ConstructWithLength(Data, Len, Str); }
FString::FString(int32 Len, const UCS2CHAR* Str)					{ UE::String::Private::ConstructWithLength(Data, Len, Str); }
FString::FString(const ANSICHAR* Str, int32 ExtraSlack)				{ UE::String::Private::ConstructWithSlack(Data, Str, ExtraSlack); }
FString::FString(const WIDECHAR* Str, int32 ExtraSlack)				{ UE::String::Private::ConstructWithSlack(Data, Str, ExtraSlack); }
FString::FString(const UTF8CHAR* Str, int32 ExtraSlack)				{ UE::String::Private::ConstructWithSlack(Data, Str, ExtraSlack); }
FString::FString(const UCS2CHAR* Str, int32 ExtraSlack)				{ UE::String::Private::ConstructWithSlack(Data, Str, ExtraSlack); }

FString& FString::operator=( const TCHAR* Other )
{
	if (Data.GetData() != Other)
	{
		int32 Len = (Other && *Other) ? FCString::Strlen(Other)+1 : 0;
		Data.Empty(Len);
		Data.AddUninitialized(Len);
			
		if( Len )
		{
			FMemory::Memcpy( Data.GetData(), Other, Len * sizeof(TCHAR) );
		}
	}
	return *this;
}


ARK_API void FString::AssignRange(const TCHAR* OtherData, int32 OtherLen)
{
	if (OtherLen == 0)
	{
		Empty();
	}
	else
	{
		const int32 ThisLen = Len();
		if (OtherLen <= ThisLen)
		{
			// Unless the input is longer, this might be assigned from a view of itself.
			TCHAR* DataPtr = Data.GetData();
			FMemory::Memmove(DataPtr, OtherData, OtherLen * sizeof(TCHAR));
			DataPtr[OtherLen] = TEXT('\0');
			Data.RemoveAt(OtherLen + 1, ThisLen - OtherLen);
		}
		else
		{
			Data.Empty(OtherLen + 1);
			Data.AddUninitialized(OtherLen + 1);
			TCHAR* DataPtr = Data.GetData();
			FMemory::Memcpy(DataPtr, OtherData, OtherLen * sizeof(TCHAR));
			DataPtr[OtherLen] = TEXT('\0');
		}
	}
}

void FString::Reserve(int32 CharacterCount)
{
	checkSlow(CharacterCount >= 0 && CharacterCount < MAX_int32);
	if (CharacterCount > 0)
	{
		Data.Reserve(CharacterCount + 1);
	}	
}

void FString::Empty(int32 Slack)
{
	Data.Empty(Slack ? Slack + 1 : 0);
}

void FString::Empty()
{
	Data.Empty(0);
}


void FString::Shrink()
{
	Data.Shrink();
}

#ifdef __OBJC__
/** Convert FString to Objective-C NSString */
NSString* FString::GetNSString() const
{
#if PLATFORM_TCHAR_IS_4_BYTES
    return [[[NSString alloc] initWithBytes:Data.GetData() length:Len() * sizeof(TCHAR) encoding:NSUTF32LittleEndianStringEncoding] autorelease];
#else
    return [[[NSString alloc] initWithBytes:Data.GetData() length:Len() * sizeof(TCHAR) encoding:NSUTF16LittleEndianStringEncoding] autorelease];
#endif
}
#endif


void FString::AppendChars(const ANSICHAR* Str, int32 Count)
{
	CheckInvariants();
	AppendCharacters(Data, Str, Count);
}

void FString::AppendChars(const WIDECHAR* Str, int32 Count)
{
	CheckInvariants();
	AppendCharacters(Data, Str, Count);
}

void FString::AppendChars(const UCS2CHAR* Str, int32 Count)
{
	CheckInvariants();
	AppendCharacters(Data, Str, Count);
}

void FString::AppendChars(const UTF8CHAR* Str, int32 Count)
{
	CheckInvariants();
	AppendCharacters(Data, Str, Count);
}

void FString::TrimToNullTerminator()
{
	if( Data.Num() )
	{
		int32 DataLen = FCString::Strlen(Data.GetData());
		check(DataLen == 0 || DataLen < Data.Num());
		int32 Len = DataLen > 0 ? DataLen+1 : 0;

		check(Len <= Data.Num());
		Data.RemoveAt(Len, Data.Num()-Len);
	}
}



bool FString::Split(const FString& InS, FString* LeftS, FString* RightS, ESearchCase::Type SearchCase, ESearchDir::Type SearchDir) const
{
	check(LeftS != RightS || LeftS == nullptr);

	int32 InPos = Find(InS, SearchCase, SearchDir);

	if (InPos < 0) { return false; }

	if (LeftS)
	{
		if (LeftS != this)
		{
			*LeftS = Left(InPos);
			if (RightS) { *RightS = Mid(InPos + InS.Len()); }
		}
		else
		{
			// we know that RightS can't be this so we can safely modify it before we deal with LeftS
			if (RightS) { *RightS = Mid(InPos + InS.Len()); }
			*LeftS = Left(InPos);
		}
	}
	else if (RightS)
	{
		*RightS = Mid(InPos + InS.Len());
	}

	return true;
}

bool FString::Split(const FString& InS, FString* LeftS, FString* RightS) const
{
	return Split(InS, LeftS, RightS, ESearchCase::IgnoreCase);
}







void FString::RemoveSpacesInline()
{
	const int32 StringLength = Len();
	if (StringLength == 0)
	{
		return;
	}

	TCHAR* RawData = Data.GetData();
	int32 CopyToIndex = 0;
	for (int32 CopyFromIndex = 0; CopyFromIndex < StringLength; ++CopyFromIndex)
	{
		if (RawData[CopyFromIndex] != ' ')
		{	// Copy any character OTHER than space.
			RawData[CopyToIndex] = RawData[CopyFromIndex];
			++CopyToIndex;
		}
	}

	// Copy null-terminating character.
	if (CopyToIndex <= StringLength)
	{
		RawData[CopyToIndex] = TEXT('\0');
		Data.SetNum(CopyToIndex + 1, false);
	}
}



void FString::InsertAt(int32 Index, TCHAR Character)
{
	if (Character != 0)
	{
		if (Data.Num() == 0)
		{
			*this += Character;
		}
		else
		{
			Data.Insert(Character, Index);
		}
	}
}

void FString::InsertAt(int32 Index, const FString& Characters)
{
	if (Characters.Len())
	{
		if (Data.Num() == 0)
		{
			*this += Characters;
		}
		else
		{
			Data.Insert(Characters.Data.GetData(), Characters.Len(), Index);
		}
	}
}

void FString::RemoveAt(int32 Index, int32 Count, bool bAllowShrinking)
{
	Data.RemoveAt(Index, FMath::Clamp(Count, 0, Len()-Index), bAllowShrinking);
}

ARK_API bool FString::RemoveFromStart(const TCHAR* InPrefix, int32 InPrefixLen, ESearchCase::Type SearchCase)
{
	if (InPrefixLen == 0 )
	{
		return false;
	}

	if (StartsWith(InPrefix, InPrefixLen, SearchCase ))
	{
		RemoveAt(0, InPrefixLen);
		return true;
	}

	return false;
}

bool FString::RemoveFromEnd(const TCHAR* InSuffix, int32 InSuffixLen, ESearchCase::Type SearchCase)
{
	if (InSuffixLen == 0)
	{
		return false;
	}

	if (EndsWith(InSuffix, InSuffixLen, SearchCase))
	{
		RemoveAt( Len() - InSuffixLen, InSuffixLen );
		return true;
	}

	return false;
}

namespace UE::String::Private
{

template <typename LhsType, typename RhsType>
UE_NODISCARD FORCEINLINE FString ConcatFStrings(LhsType&& Lhs, RhsType&& Rhs)
{
	Lhs.CheckInvariants();
	Rhs.CheckInvariants();

	if (Lhs.IsEmpty())
	{
		return Forward<RhsType>(Rhs);
	}

	int32 RhsLen = Rhs.Len();

	FString Result(Forward<LhsType>(Lhs), /* extra slack */ RhsLen);
	Result.AppendChars(Rhs.GetCharArray().GetData(), RhsLen);
		
	return Result;
}

template <typename RhsType>
UE_NODISCARD FORCEINLINE FString ConcatRangeFString(const TCHAR* Lhs, int32 LhsLen, RhsType&& Rhs)
{
	checkSlow(LhsLen >= 0);
	Rhs.CheckInvariants();
	if (LhsLen == 0)
	{
		return Forward<RhsType>(Rhs);
	}

	int32 RhsLen = Rhs.Len();

	// This is not entirely optimal, as if the Rhs is an rvalue and has enough slack space to hold Lhs, then
	// the memory could be reused here without constructing a new object.  However, until there is proof otherwise,
	// I believe this will be relatively rare and isn't worth making the code a lot more complex right now.
	FString Result;
	Result.GetCharArray().Reserve(LhsLen + RhsLen + 1);
	Result.GetCharArray().AddUninitialized(LhsLen + RhsLen + 1);

	TCHAR* ResultData = Result.GetCharArray().GetData();
	CopyAssignItems(ResultData, Lhs, LhsLen);
	CopyAssignItems(ResultData + LhsLen, Rhs.GetCharArray().GetData(), RhsLen);
	*(ResultData + LhsLen + RhsLen) = TEXT('\0');
		
	return Result;
}

template <typename LhsType>
UE_NODISCARD FORCEINLINE FString ConcatFStringRange(LhsType&& Lhs, const TCHAR* Rhs, int32 RhsLen)
{
	Lhs.CheckInvariants();
	checkSlow(RhsLen >= 0);
	if (RhsLen == 0)
	{
		return Forward<LhsType>(Lhs);
	}

	FString Result(Forward<LhsType>(Lhs), /* extra slack */ RhsLen);
	Result.AppendChars(Rhs, RhsLen);
		
	return Result;
}

template <typename RhsType>
UE_NODISCARD FORCEINLINE FString ConcatCStringFString(const TCHAR* Lhs, RhsType&& Rhs)
{
	checkSlow(Lhs);
	if (!Lhs)
	{
		return Forward<RhsType>(Rhs);
	}

	return ConcatRangeFString(Lhs, FCString::Strlen(Lhs), Forward<RhsType>(Rhs));
}

template <typename LhsType>
UE_NODISCARD FORCEINLINE FString ConcatFStringCString(LhsType&& Lhs, const TCHAR* Rhs)
{
	checkSlow(Rhs);
	if (!Rhs)
	{
		return Forward<LhsType>(Lhs);
	}
	return ConcatFStringRange(Forward<LhsType>(Lhs), Rhs, FCString::Strlen(Rhs));
}

} // namespace UE::String::Private

FString FString::ConcatFF(const FString& Lhs, const FString& Rhs)				{ return UE::String::Private::ConcatFStrings(Lhs, Rhs); }
FString FString::ConcatFF(FString&& Lhs, const FString& Rhs)					{ return UE::String::Private::ConcatFStrings(MoveTemp(Lhs), Rhs); }
FString FString::ConcatFF(const FString& Lhs, FString&& Rhs)					{ return UE::String::Private::ConcatFStrings(Lhs, MoveTemp(Rhs)); }
FString FString::ConcatFF(FString&& Lhs, FString&& Rhs)							{ return UE::String::Private::ConcatFStrings(MoveTemp(Lhs), MoveTemp(Rhs)); }
FString FString::ConcatFC(const FString& Lhs, const TCHAR* Rhs)					{ return UE::String::Private::ConcatFStringCString(Lhs, Rhs); }
FString FString::ConcatFC(FString&& Lhs, const TCHAR* Rhs)						{ return UE::String::Private::ConcatFStringCString(MoveTemp(Lhs), Rhs); }
FString FString::ConcatCF(const TCHAR* Lhs,	const FString& Rhs)					{ return UE::String::Private::ConcatCStringFString(Lhs, Rhs); }
FString FString::ConcatCF(const TCHAR* Lhs,	FString&& Rhs)						{ return UE::String::Private::ConcatCStringFString(Lhs, MoveTemp(Rhs)); }
FString FString::ConcatFR(const FString& Lhs, const TCHAR* Rhs, int32 RhsLen)	{ return UE::String::Private::ConcatFStringRange(Lhs, Rhs, RhsLen); }
FString FString::ConcatFR(FString&& Lhs, const TCHAR* Rhs, int32 RhsLen)		{ return UE::String::Private::ConcatFStringRange(MoveTemp(Lhs), Rhs, RhsLen); }
FString FString::ConcatRF(const TCHAR* Lhs, int32 LhsLen, const FString& Rhs)	{ return UE::String::Private::ConcatRangeFString(Lhs, LhsLen, Rhs); }
FString FString::ConcatRF(const TCHAR* Lhs, int32 LhsLen, FString&& Rhs)		{ return UE::String::Private::ConcatRangeFString(Lhs, LhsLen, MoveTemp(Rhs)); }

/**
 * Concatenate this path with given path ensuring the / character is used between them
 *
 * @param Str       Pointer to an array of TCHARs (not necessarily null-terminated) to be concatenated onto the end of this.
 * @param StrLength Exact number of characters from Str to append.
 */
void FString::PathAppend(const TCHAR* Str, int32 StrLength)
{
	int32 DataNum = Data.Num();
	if (StrLength == 0)
	{
		if (DataNum > 1 && Data[DataNum - 2] != TEXT('/') && Data[DataNum - 2] != TEXT('\\'))
		{
			Data[DataNum - 1] = TEXT('/');
			Data.Add(TEXT('\0'));
		}
	}
	else
	{
		if (DataNum > 0)
		{
			if (DataNum > 1 && Data[DataNum - 2] != TEXT('/') && Data[DataNum - 2] != TEXT('\\') && *Str != TEXT('/'))
			{
				Data[DataNum - 1] = TEXT('/');
			}
			else
			{
				Data.Pop(false);
				--DataNum;
			}
		}

		Reserve(DataNum + StrLength);
		Data.Append(Str, StrLength);
		Data.Add(TEXT('\0'));
	}
}




void FString::ReplaceCharInlineCaseSensitive(const TCHAR SearchChar, const TCHAR ReplacementChar)
{
	for (TCHAR& Character : Data)
	{
		Character = Character == SearchChar ? ReplacementChar : Character;
	}
}

void FString::ReplaceCharInlineIgnoreCase(const TCHAR SearchChar, const TCHAR ReplacementChar)
{
	TCHAR OtherCaseSearchChar = TChar<TCHAR>::IsUpper(SearchChar) ? TChar<TCHAR>::ToLower(SearchChar) : TChar<TCHAR>::ToUpper(SearchChar);
	ReplaceCharInlineCaseSensitive(OtherCaseSearchChar, ReplacementChar);
	ReplaceCharInlineCaseSensitive(SearchChar, ReplacementChar);
}

void FString::TrimStartAndEndInline()
{
	TrimEndInline();
	TrimStartInline();
}

FString FString::TrimStartAndEnd() const &
{
	FString Result(*this);
	Result.TrimStartAndEndInline();
	return Result;
}

FString FString::TrimStartAndEnd() &&
{
	TrimStartAndEndInline();
	return MoveTemp(*this);
}

void FString::TrimStartInline()
{
	int32 Pos = 0;
	while(Pos < Len() && FChar::IsWhitespace((*this)[Pos]))
	{
		Pos++;
	}
	RemoveAt(0, Pos);
}

FString FString::TrimStart() const &
{
	FString Result(*this);
	Result.TrimStartInline();
	return Result;
}

FString FString::TrimStart() &&
{
	TrimStartInline();
	return MoveTemp(*this);
}

void FString::TrimEndInline()
{
	int32 End = Len();
	while(End > 0 && FChar::IsWhitespace((*this)[End - 1]))
	{
		End--;
	}
	RemoveAt(End, Len() - End);
}

FString FString::TrimEnd() const &
{
	FString Result(*this);
	Result.TrimEndInline();
	return Result;
}

FString FString::TrimEnd() &&
{
	TrimEndInline();
	return MoveTemp(*this);
}

void FString::TrimCharInline(const TCHAR CharacterToTrim, bool* bCharRemoved)
{
	bool bQuotesWereRemoved=false;
	int32 Start = 0, Count = Len();
	if ( Count > 0 )
	{
		if ( (*this)[0] == CharacterToTrim )
		{
			Start++;
			Count--;
			bQuotesWereRemoved=true;
		}

		if ( Len() > 1 && (*this)[Len() - 1] == CharacterToTrim )
		{
			Count--;
			bQuotesWereRemoved=true;
		}
	}

	if ( bCharRemoved != nullptr )
	{
		*bCharRemoved = bQuotesWereRemoved;
	}
	MidInline(Start, Count, false);
}

void FString::TrimQuotesInline(bool* bQuotesRemoved)
{
	TrimCharInline(TCHAR('"'), bQuotesRemoved);
}

FString FString::TrimQuotes(bool* bQuotesRemoved) const &
{
	FString Result(*this);
	Result.TrimQuotesInline(bQuotesRemoved);
	return Result;
}

FString FString::TrimQuotes(bool* bQuotesRemoved) &&
{
	TrimQuotesInline(bQuotesRemoved);
	return MoveTemp(*this);
}

FString FString::TrimChar(const TCHAR CharacterToTrim, bool* bCharRemoved) const &
{
	FString Result(*this);
	Result.TrimCharInline(CharacterToTrim, bCharRemoved);
	return Result;
}

FString FString::TrimChar(const TCHAR CharacterToTrim, bool* bCharRemoved) &&
{
	TrimCharInline(CharacterToTrim, bCharRemoved);
	return MoveTemp(*this);
}

int32 FString::CullArray( TArray<FString>* InArray )
{
	check(InArray);
	FString Empty;
	InArray->Remove(Empty);
	return InArray->Num();
}

FString FString::Reverse() const &
{
	FString New(*this);
	New.ReverseString();
	return New;
}

FString FString::Reverse() &&
{
	ReverseString();
	return MoveTemp(*this);
}

void FString::ReverseString()
{
	if ( Len() > 0 )
	{
		TCHAR* StartChar = &(*this)[0];
		TCHAR* EndChar = &(*this)[Len()-1];
		TCHAR TempChar;
		do 
		{
			TempChar = *StartChar;	// store the current value of StartChar
			*StartChar = *EndChar;	// change the value of StartChar to the value of EndChar
			*EndChar = TempChar;	// change the value of EndChar to the character that was previously at StartChar

			StartChar++;
			EndChar--;

		} while( StartChar < EndChar );	// repeat until we've reached the midpoint of the string
	}
}

FString FString::FormatAsNumber( int32 InNumber )
{
	FString Number = FString::FromInt( InNumber ), Result;

	int32 dec = 0;
	for( int32 x = Number.Len()-1 ; x > -1 ; --x )
	{
		Result += Number.Mid(x,1);

		dec++;
		if( dec == 3 && x > 0 )
		{
			Result += TEXT(",");
			dec = 0;
		}
	}

	return Result.Reverse();
}

/**
 * Serializes a string as ANSI char array.
 *
 * @param	String			String to serialize
 * @param	Ar				Archive to serialize with
 * @param	MinCharacters	Minimum number of characters to serialize.
 */
void FString::SerializeAsANSICharArray( FArchive& Ar, int32 MinCharacters ) const
{
	int32	Length = FMath::Max( Len(), MinCharacters );
	Ar << Length;
	
	for( int32 CharIndex=0; CharIndex<Len(); CharIndex++ )
	{
		ANSICHAR AnsiChar = CharCast<ANSICHAR>( (*this)[CharIndex] );
		Ar << AnsiChar;
	}

	// Zero pad till minimum number of characters are written.
	for( int32 i=Len(); i<Length; i++ )
	{
		ANSICHAR NullChar = 0;
		Ar << NullChar;
	}
}


FString FString::FromBlob(const uint8* SrcBuffer,const uint32 SrcSize)
{
	FString Result;
	Result.Reserve( SrcSize * 3 );
	// Convert and append each byte in the buffer
	for (uint32 Count = 0; Count < SrcSize; Count++)
	{
		Result += FString::Printf(TEXT("%03d"),(uint8)SrcBuffer[Count]);
	}
	return Result;
}

bool FString::ToBlob(const FString& Source,uint8* DestBuffer,const uint32 DestSize)
{
	// Make sure the buffer is at least half the size and that the string is an
	// even number of characters long
	if (DestSize >= (uint32)(Source.Len() / 3) &&
		(Source.Len() % 3) == 0)
	{
		TCHAR ConvBuffer[4];
		ConvBuffer[3] = TEXT('\0');
		int32 WriteIndex = 0;
		// Walk the string 3 chars at a time
		for (int32 Index = 0; Index < Source.Len(); Index += 3, WriteIndex++)
		{
			ConvBuffer[0] = Source[Index];
			ConvBuffer[1] = Source[Index + 1];
			ConvBuffer[2] = Source[Index + 2];
			DestBuffer[WriteIndex] = (uint8)FCString::Atoi(ConvBuffer);
		}
		return true;
	}
	return false;
}

FString FString::FromHexBlob( const uint8* SrcBuffer, const uint32 SrcSize )
{
	FString Result;
	Result.Reserve( SrcSize * 2 );
	// Convert and append each byte in the buffer
	for (uint32 Count = 0; Count < SrcSize; Count++)
	{
		Result += FString::Printf( TEXT( "%02X" ), (uint8)SrcBuffer[Count] );
	}
	return Result;
}

bool FString::ToHexBlob( const FString& Source, uint8* DestBuffer, const uint32 DestSize )
{
	// Make sure the buffer is at least half the size and that the string is an
	// even number of characters long
	if (DestSize >= (uint32)(Source.Len() / 2) &&
		 (Source.Len() % 2) == 0)
	{
		TCHAR ConvBuffer[3];
		ConvBuffer[2] = TEXT( '\0' );
		int32 WriteIndex = 0;
		// Walk the string 2 chars at a time
		TCHAR* End = nullptr;
		for (int32 Index = 0; Index < Source.Len(); Index += 2, WriteIndex++)
		{
			ConvBuffer[0] = Source[Index];
			ConvBuffer[1] = Source[Index + 1];
			DestBuffer[WriteIndex] = (uint8)FCString::Strtoi( ConvBuffer, &End, 16 );
		}
		return true;
	}
	return false;
}

UE_DISABLE_OPTIMIZATION_SHIP
void StripNegativeZero(double& InFloat)
{
	// This works for translating a negative zero into a positive zero,
	// but if optimizations are enabled when compiling with -ffast-math
	// or /fp:fast, the compiler can strip it out.
	InFloat += 0.0f;
}
UE_ENABLE_OPTIMIZATION_SHIP

FString FString::SanitizeFloat( double InFloat, const int32 InMinFractionalDigits )
{
	// Avoids negative zero
	StripNegativeZero(InFloat);

	// First create the string
	FString TempString = FString::Printf(TEXT("%f"), InFloat);
	if (!TempString.IsNumeric())
	{
		// String did not format as a valid decimal number so avoid messing with it
		return TempString;
	}

	// Trim all trailing zeros (up-to and including the decimal separator) from the fractional part of the number
	int32 TrimIndex = INDEX_NONE;
	int32 DecimalSeparatorIndex = INDEX_NONE;
	for (int32 CharIndex = TempString.Len() - 1; CharIndex >= 0; --CharIndex)
	{
		const TCHAR Char = TempString[CharIndex];
		if (Char == TEXT('.'))
		{
			DecimalSeparatorIndex = CharIndex;
			TrimIndex = FMath::Max(TrimIndex, DecimalSeparatorIndex);
			break;
		}
		if (TrimIndex == INDEX_NONE && Char != TEXT('0'))
		{
			TrimIndex = CharIndex + 1;
		}
	}
	check(TrimIndex != INDEX_NONE && DecimalSeparatorIndex != INDEX_NONE);
	TempString.RemoveAt(TrimIndex, TempString.Len() - TrimIndex, /*bAllowShrinking*/false);

	// Pad the number back to the minimum number of fractional digits
	if (InMinFractionalDigits > 0)
	{
		if (TrimIndex == DecimalSeparatorIndex)
		{
			// Re-add the decimal separator
			TempString.AppendChar(TEXT('.'));
		}

		const int32 NumFractionalDigits = (TempString.Len() - DecimalSeparatorIndex) - 1;
		const int32 FractionalDigitsToPad = InMinFractionalDigits - NumFractionalDigits;
		if (FractionalDigitsToPad > 0)
		{
			TempString.Reserve(TempString.Len() + FractionalDigitsToPad);
			for (int32 Cx = 0; Cx < FractionalDigitsToPad; ++Cx)
			{
				TempString.AppendChar(TEXT('0'));
			}
		}
	}

	return TempString;
}

FString FString::Chr( TCHAR Ch )
{
	TCHAR Temp[2]= { Ch, TEXT('\0') };
	return FString(Temp);
}


FString FString::ChrN( int32 NumCharacters, TCHAR Char )
{
	check( NumCharacters >= 0 );

	FString Temp;
	Temp.Data.AddUninitialized(NumCharacters+1);
	for( int32 Cx = 0; Cx < NumCharacters; ++Cx )
	{
		Temp[Cx] = Char;
	}
	Temp.Data[NumCharacters] = TEXT('\0');
	return Temp;
}

FString FString::LeftPad( int32 ChCount ) const
{
	int32 Pad = ChCount - Len();

	if (Pad > 0)
	{
		return ChrN(Pad, TEXT(' ')) + *this;
	}
	else
	{
		return *this;
	}
}
FString FString::RightPad( int32 ChCount ) const
{
	int32 Pad = ChCount - Len();

	if (Pad > 0)
	{
		return *this + ChrN(Pad, TEXT(' '));
	}
	else
	{
		return *this;
	}
}



/**
 * Breaks up a delimited string into elements of a string array.
 *
 * @param	InArray		The array to fill with the string pieces
 * @param	pchDelim	The string to delimit on
 * @param	InCullEmpty	If 1, empty strings are not added to the array
 *
 * @return	The number of elements in InArray
 */
ARK_API int32 FString::ParseIntoArray( TArray<FString>& OutArray, const TCHAR* pchDelim, const bool InCullEmpty ) const
{
	// Make sure the delimit string is not null or empty
	check(pchDelim);
	OutArray.Reset();
	const TCHAR *Start = **this;
	const int32 DelimLength = FCString::Strlen(pchDelim);
	if (Start && *Start != TEXT('\0') && DelimLength)
	{
		while( const TCHAR *At = FCString::Strstr(Start,pchDelim) )
		{
			if (!InCullEmpty || At-Start)
			{
				OutArray.Emplace(UE_PTRDIFF_TO_INT32(At-Start),Start);
			}
			Start = At + DelimLength;
		}
		if (!InCullEmpty || *Start)
		{
			OutArray.Emplace(Start);
		}

	}
	return OutArray.Num();
}

bool FString::MatchesWildcard(const TCHAR* InWildcard, int32 InWildcardLen, ESearchCase::Type SearchCase) const
{
	const TCHAR* Target = **this;
	int32        TargetLength = Len();

	if (SearchCase == ESearchCase::CaseSensitive)
	{
		return UE::Core::String::Private::MatchesWildcardRecursive<UE::Core::String::Private::FCompareCharsCaseSensitive>(Target, TargetLength, InWildcard, InWildcardLen);
	}
	else
	{
		return UE::Core::String::Private::MatchesWildcardRecursive<UE::Core::String::Private::FCompareCharsCaseInsensitive>(Target, TargetLength, InWildcard, InWildcardLen);
	}
}


/** Caution!! this routine is O(N^2) allocations...use it for parsing very short text or not at all */
ARK_API int32 FString::ParseIntoArrayWS( TArray<FString>& OutArray, const TCHAR* pchExtraDelim, bool InCullEmpty ) const
{
	// default array of White Spaces, the last entry can be replaced with the optional pchExtraDelim string
	// (if you want to split on white space and another character)
	const TCHAR* WhiteSpace[] = 
	{
		TEXT(" "),
		TEXT("\t"),
		TEXT("\r"),
		TEXT("\n"),
		TEXT(""),
	};

	// start with just the standard whitespaces
	int32 NumWhiteSpaces = UE_ARRAY_COUNT(WhiteSpace) - 1;
	// if we got one passed in, use that in addition
	if (pchExtraDelim && *pchExtraDelim)
	{
		WhiteSpace[NumWhiteSpaces++] = pchExtraDelim;
	}

	return ParseIntoArray(OutArray, WhiteSpace, NumWhiteSpaces, InCullEmpty);
}

ARK_API int32 FString::ParseIntoArrayLines(TArray<FString>& OutArray, bool InCullEmpty) const
{
	// default array of LineEndings
	static const TCHAR* LineEndings[] =
	{				
		TEXT("\r\n"),
		TEXT("\r"),
		TEXT("\n"),	
	};

	// start with just the standard line endings
	int32 NumLineEndings = UE_ARRAY_COUNT(LineEndings);	
	return ParseIntoArray(OutArray, LineEndings, NumLineEndings, InCullEmpty);
}

ARK_API int32 FString::ParseIntoArray(TArray<FString>& OutArray, const TCHAR* const * DelimArray, int32 NumDelims, bool InCullEmpty) const
{
	// Make sure the delimit string is not null or empty
	check(DelimArray);
	OutArray.Reset();
	const TCHAR *Start = Data.GetData();
	const int32 Length = Len();
	if (Start)
	{
		int32 SubstringBeginIndex = 0;

		// Iterate through string.
		for(int32 i = 0; i < Len();)
		{
			int32 SubstringEndIndex = INDEX_NONE;
			int32 DelimiterLength = 0;

			// Attempt each delimiter.
			for(int32 DelimIndex = 0; DelimIndex < NumDelims; ++DelimIndex)
			{
				DelimiterLength = FCString::Strlen(DelimArray[DelimIndex]);

				// If we found a delimiter...
				if (FCString::Strncmp(Start + i, DelimArray[DelimIndex], DelimiterLength) == 0)
				{
					// Mark the end of the substring.
					SubstringEndIndex = i;
					break;
				}
			}

			if (SubstringEndIndex != INDEX_NONE)
			{
				const int32 SubstringLength = SubstringEndIndex - SubstringBeginIndex;
				// If we're not culling empty strings or if we are but the string isn't empty anyways...
				if(!InCullEmpty || SubstringLength != 0)
				{
					// ... add new string from substring beginning up to the beginning of this delimiter.
					new (OutArray) FString(SubstringEndIndex - SubstringBeginIndex, Start + SubstringBeginIndex);
				}
				// Next substring begins at the end of the discovered delimiter.
				SubstringBeginIndex = SubstringEndIndex + DelimiterLength;
				i = SubstringBeginIndex;
			}
			else
			{
				++i;
			}
		}

		// Add any remaining characters after the last delimiter.
		const int32 SubstringLength = Length - SubstringBeginIndex;
		// If we're not culling empty strings or if we are but the string isn't empty anyways...
		if(!InCullEmpty || SubstringLength != 0)
		{
			// ... add new string from substring beginning up to the beginning of this delimiter.
			new (OutArray) FString(Start + SubstringBeginIndex);
		}
	}

	return OutArray.Num();
}





/**
 * Returns a copy of this string with all quote marks escaped (unless the quote is already escaped)
 */
FString FString::ReplaceQuotesWithEscapedQuotes() &&
{
	if (Contains(TEXT("\""), ESearchCase::CaseSensitive))
	{
		FString Copy(MoveTemp(*this));

		const TCHAR* pChar = *Copy;

		bool bEscaped = false;
		while ( *pChar != 0 )
		{
			if ( bEscaped )
			{
				bEscaped = false;
			}
			else if ( *pChar == TCHAR('\\') )
			{
				bEscaped = true;
			}
			else if ( *pChar == TCHAR('"') )
			{
				*this += TCHAR('\\');
			}

			*this += *pChar++;
		}
	}

	return MoveTemp(*this);
}

static const TCHAR* CharToEscapeSeqMap[][2] =
{
	// Always replace \\ first to avoid double-escaping characters
	{ TEXT("\\"), TEXT("\\\\") },
	{ TEXT("\n"), TEXT("\\n")  },
	{ TEXT("\r"), TEXT("\\r")  },
	{ TEXT("\t"), TEXT("\\t")  },
	{ TEXT("\'"), TEXT("\\'")  },
	{ TEXT("\""), TEXT("\\\"") }
};
//int32 FString::ReplaceInline(const TCHAR* SearchText, const TCHAR* ReplacementText, ESearchCase::Type SearchCase);
static const uint32 MaxSupportedEscapeChars = UE_ARRAY_COUNT(CharToEscapeSeqMap);

void FString::ReplaceCharWithEscapedCharInline(const TArray<TCHAR>* Chars/*=nullptr*/)
{
	if ( Len() > 0 && (Chars == nullptr || Chars->Num() > 0) )
	{
		for ( int32 ChIdx = 0; ChIdx < MaxSupportedEscapeChars; ChIdx++ )
		{
			if ( Chars == nullptr || Chars->Contains(*(CharToEscapeSeqMap[ChIdx][0])) )
			{
				// use ReplaceInline as that won't create a copy of the string if the character isn't found
				ReplaceInline(CharToEscapeSeqMap[ChIdx][0], CharToEscapeSeqMap[ChIdx][1]);
			}
		}
	}
}

void FString::ReplaceEscapedCharWithCharInline(const TArray<TCHAR>* Chars/*=nullptr*/)
{
	if ( Len() > 0 && (Chars == nullptr || Chars->Num() > 0) )
	{
		// Spin CharToEscapeSeqMap backwards to ensure we're doing the inverse of ReplaceCharWithEscapedChar
		for ( int32 ChIdx = MaxSupportedEscapeChars - 1; ChIdx >= 0; ChIdx-- )
		{
			if ( Chars == nullptr || Chars->Contains(*(CharToEscapeSeqMap[ChIdx][0])) )
			{
				// use ReplaceInline as that won't create a copy of the string if the character isn't found
				ReplaceInline(CharToEscapeSeqMap[ChIdx][1], CharToEscapeSeqMap[ChIdx][0]);
			}
		}
	}
}

/** 
 * Replaces all instances of '\t' with TabWidth number of spaces
 * @param InSpacesPerTab - Number of spaces that a tab represents
 */
void FString::ConvertTabsToSpacesInline(const int32 InSpacesPerTab)
{
	//must call this with at least 1 space so the modulus operation works
	check(InSpacesPerTab > 0);

	int32 TabIndex;
	while ((TabIndex = Find(TEXT("\t"), ESearchCase::CaseSensitive)) != INDEX_NONE )
	{
		FString RightSide = Mid(TabIndex+1);
		LeftInline(TabIndex, false);

		//for a tab size of 4, 
		int32 LineBegin = Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromEnd, TabIndex);
		if (LineBegin == INDEX_NONE)
		{
			LineBegin = 0;
		}
		const int32 CharactersOnLine = (Len()-LineBegin);

		int32 NumSpacesForTab = InSpacesPerTab - (CharactersOnLine % InSpacesPerTab);
		for (int32 i = 0; i < NumSpacesForTab; ++i)
		{
			AppendChar(TEXT(' '));
		}
		Append(RightSide);
	}
}

// This starting size catches 99.97% of printf calls - there are about 700k printf calls per level
#define STARTING_BUFFER_SIZE		512

//FString FString::PrintfImpl(const TCHAR* Fmt, ...);

void FString::AppendfImpl(FString& AppendToMe, const TCHAR* Fmt, ...)
{
	int32		BufferSize = STARTING_BUFFER_SIZE;
	TCHAR	StartingBuffer[STARTING_BUFFER_SIZE];
	TCHAR*	Buffer = StartingBuffer;
	int32		Result = -1;

	// First try to print to a stack allocated location 
	GET_VARARGS_RESULT(Buffer, BufferSize, BufferSize - 1, Fmt, Fmt, Result);

	// If that fails, start allocating regular memory
	if (Result == -1)
	{
		Buffer = nullptr;
		while (Result == -1)
		{
			BufferSize *= 2;
			Buffer = (TCHAR*)FMemory::Realloc(Buffer, BufferSize * sizeof(TCHAR));
			GET_VARARGS_RESULT(Buffer, BufferSize, BufferSize - 1, Fmt, Fmt, Result);
		};
	}

	Buffer[Result] = TEXT('\0');

	AppendToMe += Buffer;

	if (BufferSize != STARTING_BUFFER_SIZE)
	{
		FMemory::Free(Buffer);
	}
}

static_assert(PLATFORM_LITTLE_ENDIAN, "FString serialization needs updating to support big-endian platforms!");

FArchive& operator<<( FArchive& Ar, FString& A )
{
	// > 0 for ANSICHAR, < 0 for UTF16CHAR serialization
	static_assert(sizeof(UTF16CHAR) == sizeof(UCS2CHAR), "UTF16CHAR and UCS2CHAR are assumed to be the same size!");

	if (Ar.IsLoading())
	{
		int32 SaveNum = 0;
		Ar << SaveNum;

		bool bLoadUnicodeChar = SaveNum < 0;
		if (bLoadUnicodeChar)
		{
			// If SaveNum cannot be negated due to integer overflow, Ar is corrupted.
			if (SaveNum == MIN_int32)
			{
				Ar.SetCriticalError();
				//UE_LOG(LogCore, Error, TEXT("Archive is corrupted"));
				return Ar;
			}

			SaveNum = -SaveNum;
		}

		int64 MaxSerializeSize = Ar.GetMaxSerializeSize();
		// Protect against network packets allocating too much memory
		if ((MaxSerializeSize > 0) && (SaveNum > MaxSerializeSize))
		{
			Ar.SetCriticalError();
			//UE_LOG(LogCore, Error, TEXT("String is too large (Size: %i, Max: %i)"), SaveNum, MaxSerializeSize);
			return Ar;
		}

		// Resize the array only if it passes the above tests to prevent rogue packets from crashing
		A.Data.Empty           (SaveNum);
		A.Data.AddUninitialized(SaveNum);

		if (SaveNum)
		{
			if (bLoadUnicodeChar)
			{
				// read in the unicode string
				auto Passthru = StringMemoryPassthru<UCS2CHAR>(A.Data.GetData(), SaveNum, SaveNum);
				Ar.Serialize(Passthru.Get(), SaveNum * sizeof(UCS2CHAR));
				if (Ar.IsByteSwapping())
				{
					for (int32 CharIndex = 0; CharIndex < SaveNum; ++CharIndex)
					{
						Passthru.Get()[CharIndex] = ByteSwap(Passthru.Get()[CharIndex]);
					}
				}
				// Ensure the string has a null terminator
				Passthru.Get()[SaveNum-1] = '\0';
				Passthru.Apply();

				// Inline combine any surrogate pairs in the data when loading into a UTF-32 string
				StringConv::InlineCombineSurrogates(A);

				// Since Microsoft's vsnwprintf implementation raises an invalid parameter warning
				// with a character of 0xffff, scan for it and terminate the string there.
				// 0xffff isn't an actual Unicode character anyway.
				int Index = 0;
				if(A.FindChar(0xffff, Index))
				{
					A[Index] = TEXT('\0');
					A.TrimToNullTerminator();
				}
			}
			else
			{
				auto Passthru = StringMemoryPassthru<ANSICHAR>(A.Data.GetData(), SaveNum, SaveNum);
				Ar.Serialize(Passthru.Get(), SaveNum * sizeof(ANSICHAR));
				// Ensure the string has a null terminator
				Passthru.Get()[SaveNum-1] = '\0';
				Passthru.Apply();
			}

			// Throw away empty string.
			if (SaveNum == 1)
			{
				A.Data.Empty();
			}
		}
	}
	else
	{
		A.Data.CountBytes(Ar);

		const bool bSaveUnicodeChar = Ar.IsForcingUnicode() || !FCString::IsPureAnsi(*A);
		if (bSaveUnicodeChar)
		{
			// Note: This is a no-op on platforms that are using a 16-bit TCHAR
 			FTCHARToUTF16 UTF16String(*A, A.Len() + 1); // include the null terminator
			int32 Num = UTF16String.Length() + 1; // include the null terminator

			int32 SaveNum = -Num;
			Ar << SaveNum;

			if (Num)
			{
				if (!Ar.IsByteSwapping())
				{
					Ar.Serialize((void*)UTF16String.Get(), sizeof(UTF16CHAR) * Num);
				}
				else
				{
					TArray<UTF16CHAR> Swapped(UTF16String.Get(), Num);
					for (int32 CharIndex = 0; CharIndex < Num; ++CharIndex)
					{
						Swapped[CharIndex] = ByteSwap(Swapped[CharIndex]);
					}
					Ar.Serialize((void*)Swapped.GetData(), sizeof(UTF16CHAR) * Num);
				}
			}
		}
		else
		{
			int32 Num = A.Data.Num();
			Ar << Num;

			if (Num)
			{
				Ar.Serialize((void*)StringCast<ANSICHAR>(A.Data.GetData(), Num).Get(), sizeof(ANSICHAR) * Num);
			}
		}
	}

	return Ar;
}
template <typename CharType>
int32 HexToBytes(const FString& HexString, uint8* OutBytes)
{
	FWideStringView Hex(HexString);
	const int32 HexCount = Hex.Len();
	const CharType* HexPos = Hex.GetData();
	const CharType* HexEnd = HexPos + HexCount;
	uint8* OutPos = OutBytes;
	if (const bool bPadNibble = (HexCount % 2) == 1)
	{
		*OutPos++ = TCharToNibble(*HexPos++);
	}
	while (HexPos != HexEnd)
	{
		const uint8 HiNibble = uint8(TCharToNibble(*HexPos++) << 4);
		*OutPos++ = HiNibble | TCharToNibble(*HexPos++);
	}
	return static_cast<int32>(OutPos - OutBytes);
}

int32 FindMatchingClosingParenthesis(const FString& TargetString, const int32 StartSearch)
{
	check(StartSearch >= 0 && StartSearch <= TargetString.Len());// Check for usage, we do not accept INDEX_NONE like other string functions

	const TCHAR* const StartPosition = (*TargetString) + StartSearch;
	const TCHAR* CurrPosition = StartPosition;
	int32 ParenthesisCount = 0;

	// Move to first open parenthesis
	while (*CurrPosition != 0 && *CurrPosition != TEXT('('))
	{
		++CurrPosition;
	}

	// Did we find the open parenthesis
	if (*CurrPosition == TEXT('('))
	{
		++ParenthesisCount;
		++CurrPosition;

		while (*CurrPosition != 0 && ParenthesisCount > 0)
		{
			if (*CurrPosition == TEXT('('))
			{
				++ParenthesisCount;
			}
			else if (*CurrPosition == TEXT(')'))
			{
				--ParenthesisCount;
			}
			++CurrPosition;
		}

		// Did we find the matching close parenthesis
		if (ParenthesisCount == 0 && *(CurrPosition - 1) == TEXT(')'))
		{
			return StartSearch + UE_PTRDIFF_TO_INT32((CurrPosition - 1) - StartPosition);
		}
	}

	return INDEX_NONE;
}

FString SlugStringForValidName(const FString& DisplayString, const TCHAR* ReplaceWith /*= TEXT("")*/)
{
	FString GeneratedName = DisplayString;

	// Convert the display label, which may consist of just about any possible character, into a
	// suitable name for a UObject (remove whitespace, certain symbols, etc.)
	{
		for (int32 BadCharacterIndex = 0; BadCharacterIndex < UE_ARRAY_COUNT(TEXT("\"' ,/.:|&!~\n\r\t@#(){}[]=;^%$`")) - 1; ++BadCharacterIndex)
		{
			const TCHAR TestChar[2] = { TEXT("\"' ,/.:|&!~\n\r\t@#(){}[]=;^%$`")[BadCharacterIndex], TEXT('\0') };
			const int32 NumReplacedChars = GeneratedName.ReplaceInline(TestChar, ReplaceWith);
		}
	}

	return GeneratedName;
}

void FTextRange::CalculateLineRangesFromString(const FString& Input, TArray<FTextRange>& LineRanges)
{
	int32 LineBeginIndex = 0;

	// Loop through splitting at new-lines
	const TCHAR* const InputStart = *Input;
	for (const TCHAR* CurrentChar = InputStart; CurrentChar && *CurrentChar; ++CurrentChar)
	{
		// Handle a chain of \r\n slightly differently to stop the FChar::IsLinebreak adding two separate new-lines
		const bool bIsWindowsNewLine = (*CurrentChar == '\r' && *(CurrentChar + 1) == '\n');
		if (bIsWindowsNewLine || FChar::IsLinebreak(*CurrentChar))
		{
			const int32 LineEndIndex = UE_PTRDIFF_TO_INT32(CurrentChar - InputStart);
			check(LineEndIndex >= LineBeginIndex);
			LineRanges.Emplace(FTextRange(LineBeginIndex, LineEndIndex));

			if (bIsWindowsNewLine)
			{
				++CurrentChar; // skip the \n of the \r\n chain
			}
			LineBeginIndex = UE_PTRDIFF_TO_INT32(CurrentChar - InputStart) + 1; // The next line begins after the end of the current line
		}
	}

	// Process any remaining string after the last new-line
	if (LineBeginIndex <= Input.Len())
	{
		LineRanges.Emplace(FTextRange(LineBeginIndex, Input.Len()));
	}
}

void StringConv::InlineCombineSurrogates(FString& Str)
{
	InlineCombineSurrogates_Array(Str.GetCharArray());
}
