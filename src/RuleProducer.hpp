#include "AppSecPolicy.hpp"
#include "SecPolicy.hpp"
#pragma once

namespace AppSecPolicy
{
	class RuleProducer
	{
	public:
		RuleProducer() = default;

		void ProduceRules()
		{
			fs::path dir;
			bool dirsLeft;
			DirInfo dirInfo;
			uintmax_t fileSize;
			std::string fileName;
			std::string extension;
			
			moodycamel::ProducerToken dirPtok(SecPolicy::dirItQueue);
			moodycamel::ConsumerToken dirCtok(SecPolicy::dirItQueue);
			moodycamel::ProducerToken fileCheckPtok(SecPolicy::fileCheckQueue);

			while (SecPolicy::dirItQueue.try_dequeue(dirCtok, dirInfo))
			{
				dirsLeft = true;
				dir = std::move(dirInfo.first);
				fileSize = dirInfo.second;
				for (const auto &currFile : fs::directory_iterator(dir))
				{
					if (fs::exists(currFile))
					{
						if (fs::is_directory(currFile))
							SecPolicy::dirItQueue.enqueue(dirPtok,
								std::move(std::make_pair(currFile, fileSize)));

						else
						{
							fileSize = fs::file_size(currFile);
							if (fileSize && fs::is_regular_file(currFile))
							{
								extension = currFile.path().extension().string();

								if (!extension.empty())
								{
									fileName = currFile.path().string();
									std::transform(fileName.begin(), fileName.end(),
										fileName.begin(), tolower);

									extension = extension.substr(1, extension.length());
									std::transform(extension.begin(), extension.end(),
										extension.begin(), toupper);

									SecPolicy::fileCheckQueue.enqueue(fileCheckPtok,
										std::move(std::make_tuple(fileName, extension, fileSize)));
								}
							}
						}
					}
				}
			}

			SecPolicy::doneProducers++;
		}
	};
}