#include "preprocess.h"

#include <catboost/libs/helpers/exception.h>
#include <catboost/libs/helpers/permutation.h>
#include <catboost/libs/helpers/progress_helper.h>
#include <catboost/libs/options/defaults_helper.h>
#include <catboost/libs/loggers/catboost_logger_helpers.h>
#include <catboost/libs/logging/logging.h>

#include <library/json/json_reader.h>

#include <util/generic/xrange.h>
#include <util/system/fs.h>


using namespace NCB;


static void CheckTestBaseline(const TVector<TConstArrayRef<float>>& trainBaseline,
                              const TVector<TConstArrayRef<float>>& testBaseline,
                              size_t testIdx) {
    size_t testDocs = testBaseline.size() ? testBaseline[0].size() : 0;
    bool trainHasBaseline = trainBaseline.ysize() != 0;
    bool testHasBaseline = testDocs == 0 ? trainHasBaseline : testBaseline.ysize() != 0;
    if (trainHasBaseline) {
        CB_ENSURE(testHasBaseline, "Baseline for test is not provided");
    }
    if (testHasBaseline) {
        CB_ENSURE(trainHasBaseline, "Baseline for train is not provided");
    }
    if (testDocs != 0) {
        CB_ENSURE(
            trainBaseline.ysize() == testBaseline.ysize(),
            "Baseline dimension differ: train: " << trainBaseline.ysize() << " vs test["
             << testIdx << "]: " << testBaseline.ysize()
        );
    }
}


void CheckConsistency(const TTrainingDataProviders& data) {
    auto learnBaseline = GetBaseline(data.Learn->TargetData);
    for (auto testIdx : xrange(data.Test.size())) {
        CheckTestBaseline(learnBaseline, GetBaseline(data.Test[testIdx]->TargetData), testIdx);
    }
}

void UpdateUndefinedRandomSeed(ETaskType taskType,
                               const NCatboostOptions::TOutputFilesOptions& outputOptions,
                               NJson::TJsonValue* updatedJsonParams,
                               std::function<void(TIFStream*, TString&)> paramsLoader) {
    const TString snapshotFilename = TOutputFiles::AlignFilePath(outputOptions.GetTrainDir(), outputOptions.GetSnapshotFilename(), /*namePrefix=*/"");
    if (outputOptions.SaveSnapshot() && NFs::Exists(snapshotFilename)) {
        TString serializedTrainParams;
        NJson::TJsonValue restoredJsonParams;
        try {
            TProgressHelper(ToString(taskType)).CheckedLoad(snapshotFilename, [&](TIFStream* inputStream) {
                paramsLoader(inputStream, serializedTrainParams);
            });
            ReadJsonTree(serializedTrainParams, &restoredJsonParams);
            CB_ENSURE(restoredJsonParams.Has("random_seed"), "Snapshot is broken.");
        } catch (const TCatBoostException&) {
            throw;
        } catch (...) {
            CATBOOST_WARNING_LOG << "Can't load progress from snapshot file: " << snapshotFilename <<
                    " Exception: " << CurrentExceptionMessage() << Endl;
            return;
        }

        if (!(*updatedJsonParams)["flat_params"].Has("random_seed") && !restoredJsonParams["flat_params"].Has("random_seed")) {
            (*updatedJsonParams)["random_seed"] = restoredJsonParams["random_seed"];
        }
    }
}

void UpdateUndefinedClassNames(const NCatboostOptions::TDataProcessingOptions& dataProcessingOptions,
                               NJson::TJsonValue* updatedJsonParams) {
    NJson::TJsonValue dataProcessingParams;
    dataProcessingOptions.Save(&dataProcessingParams);
    if (!updatedJsonParams->Has("data_processing_options")) {
        updatedJsonParams->InsertValue("data_processing_options", NJson::TJsonValue());
    }
    (*updatedJsonParams)["data_processing_options"].InsertValue("class_names", dataProcessingParams["class_names"]);
}


TDataProviderPtr ReorderByTimestampLearnDataIfNeeded(const NCatboostOptions::TCatBoostOptions& catBoostOptions,
                                                     TDataProviderPtr learnData,
                                                     NPar::TLocalExecutor* localExecutor) {

    if (catBoostOptions.DataProcessingOptions->HasTimeFlag &&
        learnData->MetaInfo.HasTimestamp &&
        (learnData->ObjectsData->GetOrder() != EObjectsOrder::Ordered))
    {
        auto objectsGrouping = learnData->ObjectsData->GetObjectsGrouping();

        // TODO(akhropov): Allow if all objects in each group have the same timestamp
        CB_ENSURE(
            objectsGrouping->IsTrivial(),
            "Reordering grouped data by timestamp is not supported yet"
        );

        auto objectsPermutation = CreateOrderByKey<ui32>(*learnData->ObjectsData->GetTimestamp());

        return learnData->GetSubset(
            GetSubset(
                objectsGrouping,
                TArraySubsetIndexing<ui32>(std::move(objectsPermutation)),
                EObjectsOrder::Ordered
            ),
            localExecutor
        );
    }
    return learnData;
}


static bool NeedShuffle(const ui32 catFeatureCount,
                        const ui32 docCount,
                        const NCatboostOptions::TCatBoostOptions& catBoostOptions) {
    if (catBoostOptions.DataProcessingOptions->HasTimeFlag) {
        return false;
    }
    // TODO(akhropov): make it universal ?
    if (catBoostOptions.GetTaskType() == ETaskType::CPU) {
        return true;
    }

    if (catFeatureCount == 0) {
        auto boostingType = catBoostOptions.BoostingOptions->BoostingType;
        UpdateBoostingTypeOption(docCount,
                                 &boostingType);
        if (boostingType ==  EBoostingType::Ordered) {
            return true;
        } else {
            return false;
        }
    } else {
        return true;
    }
}

TDataProviderPtr ShuffleLearnDataIfNeeded(const NCatboostOptions::TCatBoostOptions& catBoostOptions,
                                          TDataProviderPtr learnData,
                                          NPar::TLocalExecutor* localExecutor,
                                          TRestorableFastRng64* rand) {

    if (NeedShuffle(
            learnData->MetaInfo.FeaturesLayout->GetCatFeatureCount(),
            learnData->ObjectsData->GetObjectCount(),
            catBoostOptions))
    {
        auto objectsGroupingSubset = NCB::Shuffle(learnData->ObjectsGrouping, 1, rand);
        return learnData->GetSubset(objectsGroupingSubset, localExecutor);
    }
    return learnData;
}
