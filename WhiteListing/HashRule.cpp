#include "HashRule.hpp"
#include "WinReg.hpp"
#include "AppSecPolicy.hpp"

#include "Windows.h"
#include "Strsafe.h"
#include "Rpc.h"

//nessesary for md5 to function 
#define CRYPTOPP_ENABLE_NAMESPACE_WEAK 1

#include "Crypto++\hex.h"
#include "Crypto++\files.h"
#include "Crypto++\filters.h"
#include "Crypto++\md5.h"
#include "Crypto++\sha.h"

#include <string>
#include <vector>

#pragma comment(lib, "Version.lib")

using namespace std;
using namespace AppSecPolicy;

void HashRule::CreateNewHashRule(string fileName, SecOptions secOption)
{
	EnumFriendlyName(fileName);
	EnumFileTime();
	HashDigests(fileName);
	CreateGUID();
	WriteToRegistry(secOption);

	friendlyName.clear();
}

void HashRule::EnumFileVersion(string fileName)
{
	//Code adapted from crashmstr at
	//https://stackoverflow.com/questions/940707/how-do-i-programmatically-get-the-version-of-a-dll-or-exe-file

	LPCTSTR szVersionFile = fileName.c_str();
	DWORD  verHandle = 0;
	UINT   size = 0;
	LPBYTE lpBuffer = NULL;
	DWORD  verSize = GetFileVersionInfoSize(szVersionFile, &verHandle);

	if (verSize != NULL)
	{
		LPSTR verData = new char[verSize];

		if (GetFileVersionInfo(szVersionFile, verHandle, verSize, verData))
		{
			if (VerQueryValue(verData, TEXT("\\"), (VOID FAR* FAR*)&lpBuffer, &size))
			{
				if (size)
				{
					VS_FIXEDFILEINFO *verInfo = (VS_FIXEDFILEINFO *)lpBuffer;
					if (verInfo->dwSignature == 0xfeef04bd)
					{
						char s[50];
						// Doesn't matter if you are on 32 bit or 64 bit,
						// DWORD is always 32 bits, so first two revision numbers
						// come from dwFileVersionMS, last two come from dwFileVersionLS
						snprintf(s, 50, "%d.%d.%d.%d",
							(verInfo->dwFileVersionMS >> 16) & 0xffff,
							(verInfo->dwFileVersionMS >> 0) & 0xffff,
							(verInfo->dwFileVersionLS >> 16) & 0xffff,
							(verInfo->dwFileVersionLS >> 0) & 0xffff
						);

						fileVersion = s;
						fileVersion = " (" + fileVersion + ")";
					}
				}
			}
		}
		delete[] verData;
	}

}

//Generates FriendlyName, which is a collection of metadata from the file
void HashRule::EnumFriendlyName(string fileName)
{
	//Adapted from Henri Hein at
	//http://www.codeguru.com/cpp/w-p/win32/versioning/article.php/c4539/Versioning-in-Windows.htm
	
	EnumItemSize(fileName);

	LPCTSTR szFile = fileName.c_str();
	DWORD dwLen, dwUseless;
	LPTSTR lpVI = NULL;
	UINT verMajor;

	dwLen = GetFileVersionInfoSize(szFile, &dwUseless);
	if (dwLen == 0)
	{
		//if file has no metadata, use alternate method of generating FriendlyName
		string originalName = fileName.substr(
			fileName.rfind("\\") + 1,
			fileName.length());
		int sizeOnDisk = (4096 * ((itemSize + 4096 - 1) / 4096)) / 1024;
		
		WIN32_FIND_DATA data;
		HANDLE h = FindFirstFile(fileName.c_str(), &data);
		
		SYSTEMTIME sysTime;
		FileTimeToSystemTime(&data.ftLastWriteTime, &sysTime);
		string lastModified = to_string(sysTime.wMonth) + "/"
			+ to_string(sysTime.wDay) + "/" + to_string(sysTime.wYear)
			+ " " + to_string(sysTime.wHour - 4) + ":" 
			+ to_string(sysTime.wMinute) + ":" + to_string(sysTime.wSecond);

		friendlyName = originalName + to_string(sizeOnDisk) + "  KB" + lastModified;
	}
	else
	{
		lpVI = (LPTSTR)GlobalAlloc(GPTR, dwLen);
		if (lpVI)
		{
			EnumFileVersion(fileName);

			DWORD dwBufSize;
			VS_FIXEDFILEINFO* lpFFI;
			BOOL bRet = FALSE;
			WORD* langInfo;
			PUINT cbLang = 0;
			TCHAR tszVerStrName[128];
			LPVOID lpt;
			PUINT cbBufSize = 0;
			string temp[6];

			GetFileVersionInfo((LPTSTR)szFile, NULL, dwLen, lpVI);

			if (VerQueryValue(lpVI, TEXT("\\"),
				(LPVOID *)&lpFFI, (UINT *)&dwBufSize))
			{
				//We now have the VS_FIXEDFILEINFO in lpFFI
				verMajor = HIWORD(lpFFI->dwFileVersionMS);
			}

			VerQueryValue(lpVI, TEXT("\\VarFileInfo\\Translation"),
				(LPVOID*)&langInfo, cbLang);

			for (int i = 0; i < 5; i++)
			{
				//Prepare the label to get the metadata types in fileProps
				temp[i] = "\\StringFileInfo\\%04x%04x\\" + fileProps[i];
				StringCchPrintf(tszVerStrName, STRSAFE_MAX_CCH,
					TEXT(temp[i].c_str()),
					langInfo[0], langInfo[1]);
				//Get the string from the resource data
				if (VerQueryValue(lpVI, tszVerStrName, &lpt, cbBufSize))
					temp[i].assign((LPTSTR)lpt);    //*must* save this
			}
			//format FriendlyName
			friendlyName = temp[0] + fileVersion;
			for (int i = 1; i < 5; i++)
				friendlyName += temp[i];

			//Cleanup
			GlobalFree((HGLOBAL)lpVI);
		}
	}
}

void HashRule::EnumItemSize(string fileName)
{
	WIN32_FIND_DATA data;
	HANDLE h = FindFirstFile(fileName.c_str(), &data);
	if (h == INVALID_HANDLE_VALUE)
		//return;

	FindClose(h);

	itemSize = data.nFileSizeLow | (long long)data.nFileSizeHigh << 32;
}

//gets current time 
void HashRule::EnumFileTime()
{
	FILETIME currTime;
	GetSystemTimeAsFileTime(&currTime);

	lastModified = currTime.dwLowDateTime |
		(long long)currTime.dwHighDateTime << 32;
}

void HashRule::HashDigests(string fileName)
{
	CryptoPP::Weak::MD5 md5Hash;
	CryptoPP::SHA256 shaHash;
	string md5Digest;
	string shaDigest;

	CryptoPP::FileSource(
		fileName.c_str(), true, new CryptoPP::HashFilter(
			md5Hash, new CryptoPP::HexEncoder(new CryptoPP::StringSink(md5Digest))));

	CryptoPP::FileSource(
		fileName.c_str(), true, new CryptoPP::HashFilter(
			shaHash, new CryptoPP::HexEncoder(new CryptoPP::StringSink(shaDigest))));

	//convert string to format that can be loaded into registry
	itemData = move(convertStrToByte(md5Digest));
	sha256Hash = move(convertStrToByte(shaDigest));
}

//converts string of hex into bytes
inline vector<BYTE> HashRule::convertStrToByte(string str) noexcept
{
	vector<BYTE> vec;
	for (int i = 0; i < str.length(); i += 2)
	{
		// Convert hex char to byte
		if (str[i] >= '0' && str[i] <= '9') str[i] -= '0';
		else str[i] -= 55;  // 55 = 'str[i]' - 10
		if (str[i + 1] >= '0' && str[i + 1] <= '9') str[i + 1] -= '0';
		else str[i + 1] -= 55;

		vec.push_back((str[i] << 4) | str[i + 1]);
	}
	return vec;
}

void HashRule::CreateGUID()
{
	//make new GUID in the small chance that CoCreateGuid fails
	bool goodGUID;
	do {
		goodGUID = MakeGUID();
	} while (!goodGUID);
}

bool HashRule::MakeGUID()
{
	bool result;
	GUID rGUID;
	wchar_t szGuidW[40] = { 0 };
	char szGuidA[40] = { 0 };
	HRESULT hr = CoCreateGuid(&rGUID);
	if (hr == S_OK)
	{
		StringFromGUID2(rGUID, szGuidW, 40);
		WideCharToMultiByte(CP_ACP, 0, szGuidW, -1, szGuidA, 40, NULL, NULL);
		
		guid = szGuidA;
		for (int i = 0; i < guid.length(); i++)
		{
			if (isalpha(guid[i]))
				guid[i] = tolower(guid[i]);
		}

		result = true;
	}
		
	else
		result = false;

	return result;
}

void HashRule::WriteToRegistry(SecOptions policy) noexcept
{
	using namespace winreg;

	try
	{
		string ruleType;
		string policyPath =
			"\\Policies\\Microsoft\\Windows\\Safer\\CodeIdentifiers";

		if (policy == SecOptions::BLACKLIST)
			ruleType = "\\0\\Hashes\\";
		else
			ruleType = "\\262144\\Hashes\\";

		policyPath = "SOFTWARE" + policyPath + ruleType + guid;

		RegKey hashRuleKey(
			HKEY_LOCAL_MACHINE,
			policyPath,
			KEY_WRITE
		);

		hashRuleKey.SetStringValue("Description", "");
		hashRuleKey.SetStringValue("FriendlyName", friendlyName);
		hashRuleKey.SetDwordValue("HashAlg", hashAlg);
		hashRuleKey.SetBinaryValue("ItemData", itemData);
		hashRuleKey.SetQwordValue("ItemSize", itemSize);
		hashRuleKey.SetQwordValue("LastModified", lastModified);
		hashRuleKey.SetDwordValue("SaferFlags", 0);

		hashRuleKey.Close();
		hashRuleKey.Create(
			HKEY_LOCAL_MACHINE,
			policyPath + "\\SHA256",
			KEY_WRITE);

		hashRuleKey.SetDwordValue("HashAlg", shaHashAlg);
		hashRuleKey.SetBinaryValue("ItemData", sha256Hash);

		hashRuleKey.Close();
	}
	catch (const RegException &e)
	{
		cout << e.what() << endl;
	}
	catch (...)
	{
		cout << "Unknown exception" << endl;
	}
}