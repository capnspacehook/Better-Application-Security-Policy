#include "AppSecPolicy.hpp"
#include "DataFileManger.hpp"
#include "ProtectedPtr.hpp"
#include "include\WinReg.hpp"
#include "Windows.h"

#include "include\Crypto++\pwdbased.h"
#include "include\Crypto++\filters.h"
#include "include\Crypto++\osrng.h"
#include "include\Crypto++\files.h"
#include "include\Crypto++\sha.h"
#include "include\Crypto++\aes.h"
#include "include\Crypto++\gcm.h"
#include "include\Crypto++\hex.h"

#include <filesystem>
#include <algorithm>
#include <exception>
#include <iostream>
#include <sstream>
#include <utility>
#include <vector>
#include <thread>
#include <string>

using namespace std;
using namespace CryptoPP;
using namespace AppSecPolicy;
using namespace Protected_Ptr;
namespace fs = std::experimental::filesystem;

bool DataFileManager::OpenPolicyFile()
{
	string temp;
	bool goodOpen = true;

	GCM<AES>::Decryption decryptor;
	decryptor.SetKeyWithIV(kdfHash->data(), KEY_SIZE, kdfHash->data() + KEY_SIZE, KEY_SIZE);
	kdfHash.ProtectMemory(true);

	AuthenticatedDecryptionFilter adf(decryptor, new StringSink(temp),
		AuthenticatedDecryptionFilter::MAC_AT_END, TAG_SIZE);
	FileSource encPolicyFile(policyFileName.c_str(), false);
	//skip part containing the salt
	encPolicyFile.Pump(KEY_SIZE);
	encPolicyFile.Attach(new Redirector(adf));
	encPolicyFile.PumpAll();

	if (temp.substr(0, policyFileHeader.length()) != policyFileHeader)
		goodOpen = false;

	else
		if (adf.GetLastResult() != true)
		{
			goodOpen = false;
			cerr << "File modified!" << endl;
		}

	if (goodOpen)
	{
		//lock the settings file so that no other 
		//process/thread can read or modify it
		policyFileHandle = CreateFile(
			policyFileName.c_str(),
			GENERIC_READ | GENERIC_WRITE,
			NULL,
			NULL,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			NULL);

		*policyData = temp;
		temp.clear();

		istringstream iss(*policyData);

		//skip header 
		getline(iss, temp);
		//get global settings
		getline(iss, globalPolicySettings);

		if (getline(iss, temp))
		{
			while (temp.back() == '*')
			{
				temp.pop_back();
				userRuleInfo.emplace_back(temp);
				userRulePaths.emplace_back(temp.substr(
					RULE_PATH_POS, temp.length()));

				getline(iss, temp);
			}

			ruleInfo.emplace_back(temp + "\n");
			rulePaths.emplace_back(temp.substr(
				RULE_PATH_POS, temp.find("|{") - 4));

			while (getline(iss, temp))
			{
				ruleInfo.emplace_back(temp + "\n");
				rulePaths.emplace_back(temp.substr(
					RULE_PATH_POS, temp.find("|{") - 4));
			}
		}

		ClosePolicyFile();
	}

	SecureZeroMemory(&temp, sizeof(temp));

	return goodOpen;
}

void DataFileManager::ClosePolicyFile()
{
	GCM<AES>::Encryption encryptor;
	encryptor.SetKeyWithIV(kdfHash->data(), KEY_SIZE, kdfHash->data() + KEY_SIZE, KEY_SIZE);
	kdfHash.ProtectMemory(true);
	
	//place salt unencrypted in beginning of file
	FileSink file(policyFileName.c_str());
	ArraySource as(*kdfSalt, kdfSalt->size(), true,
		new Redirector(file));
	kdfSalt.ProtectMemory(true);

	if (policyData->substr(0, policyFileHeader.length()) != policyFileHeader
		&& policyData->substr(0, KEY_SIZE) != string(KEY_SIZE, 'A'))
	{
		//prepend header and global policy settings
		policyData->insert(0, policyFileHeader);
		policyData->insert(policyFileHeader.length(), GetCurrentPolicySettings() + '\n');

		//prepend dummy data that will be skipped
		policyData->insert(0, KEY_SIZE, 'A');
	}
	
	else if (policyData->substr(0, policyFileHeader.length()) == policyFileHeader)
	{
		//prepend dummy data that will be skipped
		policyData->insert(0, KEY_SIZE, 'A');
	}
	
	StringSource updatedData(*policyData, false);
	//skip part containing the salt
	updatedData.Pump(KEY_SIZE);
	updatedData.Attach(new AuthenticatedEncryptionFilter(
		encryptor, new Redirector(file), false, TAG_SIZE));
	updatedData.PumpAll();

	policyData.ProtectMemory(true);
}

string DataFileManager::GetCurrentPolicySettings() const
{
	using namespace winreg;

	try
	{
		string policySettings = "";
		RegKey policyKey(
			HKEY_LOCAL_MACHINE,
			"SOFTWARE\\Policies\\Microsoft\\Windows\\Safer\\CodeIdentifiers",
			KEY_READ);
		
		policySettings += to_string(policyKey.GetDwordValue("AuthenticodeEnabled"));
		policySettings += "|";
		
		DWORD defaultLevel = policyKey.GetDwordValue("DefaultLevel");
		if (defaultLevel == 262144)
			defaultLevel = 1;
		
		policySettings += to_string(defaultLevel);
		policySettings += "|";

		policySettings += to_string(policyKey.GetDwordValue("PolicyScope"));
		policySettings += "|";

		policySettings += to_string(policyKey.GetDwordValue("TransparentEnabled"));
		policySettings += "|";

		vector<string> exeTypes = policyKey.GetMultiStringValue("ExecutableTypes");
		for (const auto& type : exeTypes)
			policySettings += type + ",";

		policySettings.pop_back();

		return policySettings;
	}
	catch (const RegException &e)
	{
		cout << e.what() << endl;
	}
	catch (const exception &e)
	{
		cout << e.what() << endl;
	}
}

RuleFindResult DataFileManager::FindRule(SecOption option, RuleType type,
	const string &path, RuleData &foundRuleData) const
{
	SecOption findOp = option;
	RuleType findType = type;
	RuleFindResult result = RuleFindResult::NO_MATCH;

	if (rulePaths.size() == 0)
		return result;
	
	auto iterator = lower_bound(rulePaths.begin(), rulePaths.end(), path);
	
	if (iterator != rulePaths.end() && !(path < *iterator))
	{
		string foundRule = ruleInfo[distance(rulePaths.begin(), iterator)];
	
		foundRuleData = move(StringToRuleData(foundRule));

		option = get<SEC_OPTION>(foundRuleData);
		type = get<RULE_TYPE>(foundRuleData);

		if (findOp != option && findType != type)
			result = RuleFindResult::DIFF_OP_AND_TYPE;

		else if (findOp != option)
			result = RuleFindResult::DIFF_SEC_OP;

		else if (findType != type)
			result = RuleFindResult::DIFF_TYPE;

		else
			result = RuleFindResult::EXACT_MATCH;
	}

	return result;
}

RuleFindResult DataFileManager::FindUserRule(SecOption option, RuleType type,
	const string &path, size_t &index, bool &parentDirDiffOp) const
{
	bool validRule = false;
	
	bool nonExistingSubdir = false;
	SecOption findOp = option;
	SecOption parentOp;
	RuleType findType = type;
	RuleFindResult result = RuleFindResult::NO_MATCH;

	if (userRulePaths.size() == 0)
		return result;

	vector<string>::const_iterator foundRule;

	auto foundRule1 = lower_bound(userRulePaths.begin(), userRulePaths.end(), path,
		[&path](string const& str1, string const& str2)
		{
			// compare UP TO the length of the prefix and no farther
			if (auto cmp = strncmp(str1.data(), str2.data(), path.size()))
				return cmp > 0;

			// The strings are equal to the length of the suffix so
			// behave as if they are equal. That means s1 < s2 == false
			return false;
		});

	if (foundRule1 != userRulePaths.end() && !(path < *foundRule1))
	{
		if (*foundRule1 != path)
		{
			nonExistingSubdir = true;
			result = RuleFindResult::NO_EXIST_SUBDIR_SAME_OP;
			parentOp = static_cast<SecOption>(
				userRuleInfo[distance(userRulePaths.begin(), foundRule1)][SEC_OPTION] - '0');
		}

		foundRule = foundRule1;
		validRule = true;
	}

	if (validRule)
	{
		bool existingSubdir = false;

		auto foundRule2 = lower_bound(userRulePaths.begin(), userRulePaths.end(), path);

		if (foundRule2 != userRulePaths.end() && !(path < *foundRule2))
		{
			if (foundRule1 != foundRule2)
			{
				foundRule = foundRule2;
				existingSubdir = true;
				nonExistingSubdir = false;
			}
		}

		index = distance(userRulePaths.begin(), foundRule);

		string foundRuleInfo = userRuleInfo[index];

		option = static_cast<SecOption>(
			(int)(foundRuleInfo[SEC_OPTION_POS] - '0'));

		type = static_cast<RuleType>(
			(int)(foundRuleInfo[RULE_TYPE_POS] - '0'));

		if (option == SecOption::REMOVED)
			result = RuleFindResult::NO_MATCH;

		else if (nonExistingSubdir)
		{
			if (findOp != option)
				result = RuleFindResult::NO_EXIST_SUBDIR_DIFF_SEC_OP;

			else
				result = RuleFindResult::NO_EXIST_SUBDIR_SAME_OP;
		}

		else
		{
			if (findOp != option && findType != type)
				result = RuleFindResult::DIFF_OP_AND_TYPE;

			else if (findOp != option)
			{
				if (existingSubdir)
				{
					result = RuleFindResult::EXIST_SUBDIR_DIFF_OP;

					if (parentOp != option)
						parentDirDiffOp = true;
				}

				else
					result = RuleFindResult::DIFF_SEC_OP;
			}
			
			else if (findType != type)
				result = RuleFindResult::DIFF_TYPE;

			else
			{
				if (existingSubdir)
					result = RuleFindResult::EXIST_SUBDIR_SAME_OP;

				else
					result = RuleFindResult::EXACT_MATCH;
			}
		}
	}

	return result;
}

bool DataFileManager::FindRulesInDir(const string &path, vector<RuleData> &rulesInDir) const
{
	bool rulesExist = false;
	auto removedRulesBegin = lower_bound(rulePaths.begin(), rulePaths.end(), path);

	auto removedRulesEnd = upper_bound(removedRulesBegin, rulePaths.end(), path,
		[&path](string const& str1, string const& str2)
		{
			// compare UP TO the length of the prefix and no farther
			if (auto cmp = strncmp(str1.data(), str2.data(), path.size()))
				return cmp < 0;

			// The strings are equal to the length of the prefix so
			// behave as if they are equal. That means s1 < s2 == false
			return false;
		});

	if ((removedRulesBegin != rulePaths.end() || removedRulesEnd != rulePaths.end())
		&& (removedRulesBegin != rulePaths.begin() || removedRulesEnd != rulePaths.begin()))
	{
		auto ruleInfoRange = make_pair(
			ruleInfo.begin() + distance(rulePaths.begin(), removedRulesBegin),
			ruleInfo.begin() + distance(rulePaths.begin(), removedRulesEnd));

		for (auto it = ruleInfoRange.first; it != ruleInfoRange.second; ++it)
			rulesInDir.emplace_back(move(StringToRuleData(*it)));

		rulesExist = true;
	}

	return rulesExist;
}

RuleData DataFileManager::StringToRuleData(const string& ruleStr) const
{
	string temp;
	string hash;
	RuleData ruleData;
	istringstream iss(ruleStr);

	getline(iss, temp, '|');
	get<SEC_OPTION>(ruleData) = static_cast<SecOption>(
		static_cast<int>((temp.front() - '0')));

	getline(iss, temp, '|');
	get<RULE_TYPE>(ruleData) = static_cast<RuleType>(
		static_cast<int>((temp.front() - '0')));

	getline(iss, temp, '|');
	get<FILE_LOCATION>(ruleData) = temp;

	getline(iss, temp, '|');
	get<RULE_GUID>(ruleData) = temp;

	getline(iss, temp, '|');
	get<FRIENDLY_NAME>(ruleData) = temp;

	getline(iss, temp, '|');
	get<ITEM_SIZE>(ruleData) = stoull(temp);

	getline(iss, temp, '|');
	get<LAST_MODIFIED>(ruleData) = stoull(temp);

	getline(iss, temp, '|');
	StringSource(temp, true,
		new HexDecoder(new StringSink(hash)));

	for (int i = 0; i < hash.size(); i++)
		get<ITEM_DATA>(ruleData).emplace_back(hash[i]);

	hash.clear();
	getline(iss, temp, '|');
	temp.pop_back();
	StringSource(temp, true,
		new HexDecoder(new StringSink(hash)));

	for (int i = 0; i < hash.size(); i++)
		get<SHA256_HASH>(ruleData).emplace_back(hash[i]);

	get<RULE_UPDATED>(ruleData) = false;

	return ruleData;
}

string DataFileManager::RuleDataToString(const RuleData& ruleData) const
{
	auto hashToStr =
		[](const vector<BYTE>& hash)
		{
			string hashStr;
			string hexStr;
			for (const auto byte : hash)
				hashStr += byte;

			StringSource(hashStr, true,
				new HexEncoder(new StringSink(hexStr)));
			return hexStr;
		};

	return to_string(static_cast<int>(get<SEC_OPTION>(ruleData))) + '|' +
		to_string(static_cast<int>(get<RULE_TYPE>(ruleData))) + '|' + 
		get<FILE_LOCATION>(ruleData) + '|' + get<RULE_GUID>(ruleData) + '|' +
		get<FRIENDLY_NAME>(ruleData) + '|' + to_string(get<ITEM_SIZE>(ruleData)) + '|' + 
		to_string(get<LAST_MODIFIED>(ruleData)) + '|' +
		hashToStr(get<ITEM_DATA>(ruleData)) + '|' + hashToStr(get<SHA256_HASH>(ruleData)) + '\n';
}

void DataFileManager::SortRules()
{
	if (userRulesNotSorted)
	{
		sort(userRuleInfo.begin(), userRuleInfo.end(),
			[](const string &str1, const string &str2)
			{
				return str1.substr(RULE_PATH_POS,
					str1.length())
					< str2.substr(RULE_PATH_POS,
						str2.length());
			});

		userRulePaths.clear();
		for (const auto& rule : userRuleInfo)
		{
			userRulePaths.emplace_back(rule.substr(RULE_PATH_POS,
				rule.length()));
		}

		userRulesNotSorted = false;
	}

	if (rulesAdded)
	{
		sort(ruleInfo.begin(), ruleInfo.end(),
			[](const string &str1, const string &str2)
			{
				return str1.substr(RULE_PATH_POS,
					str1.find("|{") - 4)
					< str2.substr(RULE_PATH_POS,
						str2.find("|{") - 4);
			});

		rulePaths.clear();
		for (const auto& rule : ruleInfo)
		{
			rulePaths.emplace_back(rule.substr(RULE_PATH_POS,
				rule.find("|{") - 4));
		}

		rulesAdded = false;
		rulesNotSorted = false;
	}
}

void DataFileManager::UpdateUserRules(const vector<UserRule> &ruleNames, bool rulesRemoved)
{
	
	SecOption option;
	RuleType type;
	string location;
	size_t index;

	rulesNotSorted = false;
	userRulesNotSorted = false;
	bool parentDiffOp = false;

	for (const auto& rule : ruleNames)
	{
		option = static_cast<SecOption>(get<SEC_OPTION>(rule));
		type = static_cast<RuleType>(get<RULE_TYPE>(rule));
		location = get<FILE_LOCATION>(rule);

		SortRules();

		RuleFindResult result = FindUserRule(option, type, location, index, parentDiffOp);

		if (result == RuleFindResult::NO_MATCH && !rulesRemoved)
		{
			rulesNotSorted = true;
			userRuleInfo.emplace_back(to_string(static_cast<int>(option))
				+ '|' + to_string(static_cast<int>(type))
				+ '|' + location);
			
			userRulesNotSorted = true;
		}

		else if (result == RuleFindResult::EXIST_SUBDIR_DIFF_OP && !rulesRemoved)
		{
			switchedRules.emplace_back(location);

			if (parentDiffOp)
				userRuleInfo.erase(userRuleInfo.begin() + index);
		}

		else if (result != RuleFindResult::NO_MATCH && rulesRemoved)
		{
			if (result == RuleFindResult::EXIST_SUBDIR_SAME_OP
				|| result == RuleFindResult::EXIST_SUBDIR_DIFF_OP)
			{
				rulesNotSorted = true;

				removedRules.emplace_back(location);
				userRuleInfo.emplace_back(
					to_string(static_cast<int>(SecOption::REMOVED))
					+ '|' + to_string(static_cast<int>(type))
					+ '|' + location);

				userRulesNotSorted = true;
			}

			else
			{
				removedRules.emplace_back(userRulePaths[index]);
				userRuleInfo.erase(userRuleInfo.begin() + index);
			}
		}

		else if (result == RuleFindResult::NO_EXIST_SUBDIR_DIFF_SEC_OP)
		{
			switchedRules.emplace_back(location);
			userRuleInfo.emplace_back(to_string(static_cast<int>(option))
				+ '|' + to_string(static_cast<int>(type))
				+ '|' + location);

			userRulesNotSorted = true;
		}

		else if (result == RuleFindResult::DIFF_SEC_OP)
		{
			switchedRules.emplace_back(location);
			userRuleInfo[index][SEC_OPTION] = static_cast<char>(option) + '0';
		}
	}
}

void DataFileManager::InsertNewEntries(const vector<shared_ptr<RuleData>>& ruleData)
{
	try
	{
		if (ruleData.size())
			rulesAdded = true;

		for (const auto& rule : ruleData)
		{
			rulePaths.emplace_back(get<FILE_LOCATION>(*rule));
			ruleInfo.emplace_back(move(RuleDataToString(*rule)));
		}

		ReorganizePolicyData();
	}
	catch (const exception &e)
	{
		cerr << e.what() << endl;
	}
}

void DataFileManager::SwitchEntries(SecOption option)
{
	SortRules();

	for (const auto& rule : switchedRules)
	{
		auto switchedRulesBegin = lower_bound(rulePaths.begin(), rulePaths.end(), rule);

		auto switchedRulesEnd = upper_bound(switchedRulesBegin, rulePaths.end(), rule,
			[&rule](string const& str1, string const& str2)
			{
				// compare UP TO the length of the prefix and no farther
				if (auto cmp = strncmp(str1.data(), str2.data(), rule.size()))
					return cmp < 0;

				// The strings are equal to the length of the prefix so
				// behave as if they are equal. That means s1 < s2 == false
				return false;
			});

		auto ruleInfoRange = make_pair(
			ruleInfo.begin() + distance(rulePaths.begin(), switchedRulesBegin),
			ruleInfo.begin() + distance(rulePaths.begin(), switchedRulesEnd));

		for (auto it = ruleInfoRange.first; it != ruleInfoRange.second; ++it)
			it->at(SEC_OPTION) = static_cast<char>(option) + '0';
	}

	ReorganizePolicyData();
}

void DataFileManager::UpdateEntries(SecOption option,
	const vector<shared_ptr<RuleData>>& ruleData)
{
	SortRules();

	for (const auto &updatedRule : ruleData)
	{
		auto iterator = lower_bound(
			rulePaths.begin(), rulePaths.end(), get<FILE_LOCATION>(*updatedRule));

		if (iterator != rulePaths.end() && !(get<FILE_LOCATION>(*updatedRule) < *iterator))
		{
			if (get<RULE_UPDATED>(*updatedRule) == true)
				ruleInfo[distance(rulePaths.begin(), iterator)] = move(RuleDataToString(*updatedRule));

			else
				ruleInfo[distance(rulePaths.begin(), iterator)][SEC_OPTION] = static_cast<char>(option) + '0';
		}

		else
			cerr << "Updating entries failed: rule not found\n";
	}
}

void DataFileManager::RemoveOldEntries()
{
	SortRules();

	for (const auto& rule : removedRules)
	{
		auto removedRulesBegin = lower_bound(rulePaths.begin(), rulePaths.end(), rule);

		auto removedRulesEnd = upper_bound(removedRulesBegin, rulePaths.end(), rule,
			[&rule](string const& str1, string const& str2)
			{
				// compare UP TO the length of the prefix and no farther
				if (auto cmp = strncmp(str1.data(), str2.data(), rule.size()))
					return cmp < 0;

				// The strings are equal to the length of the prefix so
				// behave as if they are equal. That means s1 < s2 == false
				return false;
			});

		auto ruleInfoRange = make_pair(
			ruleInfo.begin() + distance(rulePaths.begin(), removedRulesBegin),
			ruleInfo.begin() + distance(rulePaths.begin(), removedRulesEnd));

		rulePaths.erase(removedRulesBegin, removedRulesEnd);
		ruleInfo.erase(ruleInfoRange.first, ruleInfoRange.second);
	}

	ReorganizePolicyData();
}

void DataFileManager::ReorganizePolicyData()
{
	if (userRulesNotSorted)
		sort(userRuleInfo.begin(), userRuleInfo.end(),
			[](const string &str1, const string &str2)
			{
				return str1.substr(RULE_PATH_POS,
					str1.length())
					< str2.substr(RULE_PATH_POS,
						str2.length());
			});

	if (rulesNotSorted)
	{
		sort(ruleInfo.begin(), ruleInfo.end(),
			[](const string &str1, const string &str2)
			{
				return str1.substr(RULE_PATH_POS,
					str1.find("|{") - 4)
					< str2.substr(RULE_PATH_POS,
					str2.find("|{") - 4);
			});
	}

	policyData->clear();

	for (const auto& line : userRuleInfo)
		*policyData += line + '*' + '\n';

	for (const auto& line : ruleInfo)
		*policyData += line;

	policyDataModified = true;
}

void DataFileManager::VerifyPassword(string& guessPwd)
{
	if (fs::exists(policyFileName))
		CheckPassword(guessPwd);
	else
		SetNewPassword();

	cout << endl;
}

void DataFileManager::CheckPassword(string& guessPwd)
{
	bool validPwd;
	bool cmdPwd = true;

	if (guessPwd.size() == 0)
		cmdPwd = false;
	
	//get salt from beginning of file
	FileSource encPolicyFile(policyFileName.c_str(), false);
	encPolicyFile.Attach(new ArraySink(*kdfSalt, kdfSalt->size()));
	encPolicyFile.Pump(KEY_SIZE);
	
	do
	{
		if (!cmdPwd)
		{
			cout << "Enter the password:\n";
			GetPassword(guessPwd);
		}

		cout << "Verifying password...";

		PKCS5_PBKDF2_HMAC<SHA256> kdf;
		kdf.DeriveKey(
			kdfHash->data(),
			kdfHash->size(),
			0,
			(byte*)guessPwd.data(),
			guessPwd.size(),
			kdfSalt->data(),
			kdfSalt->size(),
			iterations);

		kdfHash.ProtectMemory(true);
		kdfSalt.ProtectMemory(true);

		validPwd = OpenPolicyFile();

		if (validPwd)
			cout << "done\n";

		else
		{
			cout << "\nInvalid password entered\n";

			if (cmdPwd)
				exit(-1);
		}

	} while (!validPwd);

	SecureZeroMemory(&guessPwd, sizeof(guessPwd));
}

void DataFileManager::SetNewPassword()
{
	string newPass1, newPass2;
	bool passesMismatch = true;

	do
	{
		cout << "Enter your new password:\n";
		GetPassword(newPass1);

		cout << "Enter it again:\n";
		GetPassword(newPass2);

		if (newPass1 != newPass2)
			cout << "Passwords do not match. Please enter again.\n";
		else
			passesMismatch = false;
	} while (passesMismatch);

	cout << "Computing new password hash...";
	SecureZeroMemory(&newPass1, sizeof(newPass1));

	AutoSeededRandomPool prng;
	prng.GenerateBlock(*kdfSalt, KEY_SIZE);

	PKCS5_PBKDF2_HMAC<SHA256> kdf;
	kdf.DeriveKey(
		kdfHash->data(),
		kdfHash->size(),
		0,
		(byte*)newPass2.data(),
		newPass2.size(),
		kdfSalt->data(),
		kdfSalt->size(),
		iterations);

	SecureZeroMemory(&newPass2, sizeof(newPass2));

	kdfHash.ProtectMemory(true);
	kdfSalt.ProtectMemory(true);

	winreg::RegKey policySettings(
		HKEY_LOCAL_MACHINE,
		"SOFTWARE\\Policies\\Microsoft\\Windows\\Safer\\CodeIdentifiers",
		KEY_WRITE);

	policySettings.SetMultiStringValue("ExecutableTypes", executableTypes);

	passwordReset = true;

	cout << "done" << endl;
	ClosePolicyFile();
}

void DataFileManager::GetPassword(string &password)
{
	//set console to not show typed password
	HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
	DWORD mode = 0;
	GetConsoleMode(hStdin, &mode);
	SetConsoleMode(hStdin, mode & (~ENABLE_ECHO_INPUT));
	
	getline(cin, password);

	//reset console to normal
	SetConsoleMode(hStdin, mode);
}

void DataFileManager::ListRules() const
{
	auto sortedRules = userRuleInfo;
	sort(sortedRules.begin(), sortedRules.end(), 
		[](const string &str1, const string &str2)
	{
		if (str1[SEC_OPTION] != str2[SEC_OPTION])
			return str1[SEC_OPTION] > str2[SEC_OPTION];

		fs::path path1(str1.substr(RULE_PATH_POS,
			str1.length()));

		fs::path path2(str2.substr(RULE_PATH_POS,
			str2.length()));

		if (path1.parent_path() != path2.parent_path())
			return path1.parent_path() < path2.parent_path();

		else 
			return path1.filename() < path2.filename();
	});

	char whiteList = static_cast<char>(SecOption::WHITELIST) + '0';
	char blackList = static_cast<char>(SecOption::BLACKLIST) + '0';

	if (sortedRules.size() > 0)
	{
		unsigned index = 0;

		if (sortedRules[0][0] == whiteList)
		{
			cout << "Allowed rules:\n";
			for (index; index < sortedRules.size(); index++)
			{
				if (sortedRules[index][SEC_OPTION] == whiteList)
				{
					cout << sortedRules[index].substr(RULE_PATH_POS,
						sortedRules[index].length())
						<< "\n";
				}
				else
					break;
			}

			cout << endl;
		}

		if (sortedRules[0][0] == blackList)
		{
			cout << "Denied rules:\n";
			for (index; index < sortedRules.size(); index++)
			{
				if (sortedRules[index][SEC_OPTION] == blackList)
				{
					cout << sortedRules[index].substr(RULE_PATH_POS,
						sortedRules[index].length())
						<< "\n";
				}
				else
					break;
			}
		}
	}

	else
		cout << "No rules have been created\n";
}