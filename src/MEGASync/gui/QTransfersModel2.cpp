#include "QTransfersModel2.h"
#include "MegaApplication.h"
#include "Utilities.h"

using namespace mega;

QTransfersModel2::QTransfersModel2(QObject *parent) :
    QAbstractItemModel(parent),
    mMegaApi(((MegaApplication *)qApp)->getMegaApi()),
    mPreferences(Preferences::instance()),
    mTransfers(QMap<TransferTag,QVariant>()),
    mRemainingTimes(QMap<TransferTag, TransferRemainingTime*>()),
    mOrder(QList<TransferTag>()),
    mThreadPool(ThreadPoolSingleton::getInstance()),
    mModelMutex(QMutex::NonRecursive),
    mNotificationNumber(0)
{
    // Init File Types
    mFileTypes[Utilities::getExtensionPixmapName(QLatin1Literal("a.txt"), QString())] = TransferData::TYPE_TEXT;
    mFileTypes[Utilities::getExtensionPixmapName(QLatin1Literal("a.wav"), QString())] = TransferData::TYPE_AUDIO;
    mFileTypes[Utilities::getExtensionPixmapName(QLatin1Literal("a.mkv"), QString())] = TransferData::TYPE_VIDEO;
    mFileTypes[Utilities::getExtensionPixmapName(QLatin1Literal("a.tar"), QString())] = TransferData::TYPE_ARCHIVE;
    mFileTypes[Utilities::getExtensionPixmapName(QLatin1Literal("a.odt"), QString())] = TransferData::TYPE_DOCUMENT;
    mFileTypes[Utilities::getExtensionPixmapName(QLatin1Literal("a.png"), QString())] = TransferData::TYPE_IMAGE;
    mFileTypes[Utilities::getExtensionPixmapName(QLatin1Literal("a.bin"), QString())] = TransferData::TYPE_OTHER;

    // Connect to transfer changes signals
    mMegaApi->addTransferListener(this);

    // Connect to pause state change signal
    QObject::connect((MegaApplication *)qApp, &MegaApplication::pauseStateChanged,
                      this, &QTransfersModel2::onPauseStateChanged);

    mAreDlPaused = mPreferences->getDownloadsPaused();
    mAreUlPaused = mPreferences->getUploadsPaused();

    qRegisterMetaType<TransferData::FileTypes>("TransferData::FileTypes");
}

int QTransfersModel2::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
    {
        return 0;
    }
    return mTransfers.size();
}

int QTransfersModel2::columnCount(const QModelIndex &parent) const
{
    return 1;
}

QVariant QTransfersModel2::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || (index.row() < 0 || mTransfers.count() <= index.row()))
    {
        return QVariant();
    }

    if (role == Qt::DisplayRole)
    {
        return mTransfers[mOrder[index.row()]];
    }
    return QVariant();
}

QModelIndex QTransfersModel2::parent(const QModelIndex &) const
{
    return QModelIndex();
}

QModelIndex QTransfersModel2::index(int row, int column, const QModelIndex &parent) const
{
    if (!hasIndex(row, column, parent))//Check out
    {
        return QModelIndex();
    }

    return createIndex(row, column, mOrder.at(row));
}

QTransfersModel2::~QTransfersModel2()
{
    mMegaApi->removeTransferListener(this);
    qDeleteAll(mRemainingTimes);
}

void QTransfersModel2::initModel()
{
    QTransfersModel2* transferModel = this;

    emitStatistics();

    mThreadPool->push([this, transferModel]()
    {//thread pool function

        if (!transferModel)
        {
            return;
        }

        Utilities::queueFunctionInAppThread([this, transferModel]()
        {//queued function

            if (transferModel) //Check if this is not deleted
            {
                auto transfers (mMegaApi->getTransfers());
                auto transferData(mMegaApi->getTransferData());
                mNotificationNumber = transferData->getNotificationNumber();

                // First, list all the transfers to add
                QList<TransferTag> transfersToAdd;

                for (auto i (0); i < transfers->size(); ++i)
                {
                    mega::MegaTransfer* mt (transfers->get(i));

                    auto priority (mt->getPriority());
                    if (!mt->isStreamingTransfer()
                            && !mt->isFolderTransfer()
                            && priority)
                    {

                        // Sort transfers
                        if (transfersToAdd.isEmpty())
                        {
                            transfersToAdd.push_back(i);
                        }
                        else
                        {
                            if (priority < transfers->get(transfersToAdd.first())->getPriority())
                            {
                                transfersToAdd.push_front(i);
                            }
                            else
                            {
                                // Start from the back
                                auto otherTag = transfersToAdd.rbegin();
                                while (priority < transfers->get(*otherTag)->getPriority())
                                {
                                    otherTag++;
                                }
                                transfersToAdd.insert(*otherTag, i);
                            }
                        }
                    }
                }

                // Then load them in the model by chunks of 50
                const auto nbRows(transfersToAdd.size());
                if (nbRows > 0)
                {
                    auto remainingRows(nbRows);

                    // Load in chunks for responsiveness
                    constexpr int rowsPerChunk (50);
                    auto nbChunks (nbRows / rowsPerChunk);

                    // Add 1 chunk more if needed
                    if ((nbChunks * rowsPerChunk) < nbRows)
                    {
                        nbChunks++;
                    }

                    for (auto chunk(0); chunk < nbChunks; ++chunk)
                    {
                        auto first (nbRows - remainingRows);
                        auto nbRowsInChunk (std::min(remainingRows, rowsPerChunk));

                        mModelMutex.lock();
                        // Use the actual number of items to update the rows, in case
                        // transfers were added/removed between chunks.
                        auto nbRowsInModel (mOrder.size());
                        beginInsertRows(QModelIndex(), nbRowsInModel,
                                        nbRowsInModel + nbRowsInChunk - 1);

                        // Insert transfers
                        for (auto row (first); row < first + nbRowsInChunk; ++row)
                        {
                            insertTransfer(mMegaApi, transfers->get(transfersToAdd[row]), row);
                        }

                        endInsertRows();
                        mModelMutex.unlock();
                        emitStatistics();
                        remainingRows -= rowsPerChunk;
                    }
                }
                delete transfers;
            }
        });//end of queued function
    });// end of thread pool function
}

void QTransfersModel2::onTransferStart(mega::MegaApi *api, mega::MegaTransfer *transfer)
{
    if (transfer->isStreamingTransfer()
            || transfer->isFolderTransfer()
            || mNotificationNumber >= transfer->getNotificationNumber())
    {
        return;
    }

    mModelMutex.lock();

    auto row (mOrder.size());

    // Find place
    if (row > 0)
    {
        auto priority (transfer->getPriority());
        auto other (qvariant_cast<TransferItem2>(mTransfers[mOrder.first()]).getTransferData());
        if (priority < other->mPriority)
        {
            row = 0;
        }
        else
        {
            auto otherTag (mOrder.rbegin());
            other = qvariant_cast<TransferItem2>(mTransfers[*otherTag]).getTransferData();
            while (priority < other->mPriority && otherTag != mOrder.rend())
            {
                otherTag++;
                other = qvariant_cast<TransferItem2>(mTransfers[*otherTag]).getTransferData();
                row--;
            }
        }
    }

    beginInsertRows(QModelIndex(), row, row);
    insertTransfer(api, transfer, row);
    endInsertRows();

    mModelMutex.unlock();
    emitStatistics();

    mNotificationNumber = transfer->getNotificationNumber();
}

void QTransfersModel2::onTransferFinish(mega::MegaApi* api, mega::MegaTransfer *transfer, mega::MegaError* error)
{
    if (transfer->isStreamingTransfer()
            || transfer->isFolderTransfer()
            || mNotificationNumber >= transfer->getNotificationNumber())
    {
        return;
    }


    TransferTag tag (transfer->getTag());
    auto row (mOrder.indexOf(tag));

    if (row >= 0)
    {
        auto transferItem (static_cast<TransferItem2*>(mTransfers[tag].data()));

        auto state (transfer->getState());
        auto prevState (transferItem->getState());
        int  errorCode (MegaError::API_OK);
        auto errorValue (0LL);

        if (error)
        {
            errorCode = error->getErrorCode();
            errorValue = error->getValue();
        }

        transferItem->updateValuesTransferFinished(transfer->getUpdateTime(),
                                         errorCode,
                                         errorValue,
                                         transfer->getMeanSpeed(),
                                         state,
                                         transfer->getTransferredBytes());

        emit dataChanged(index(row, 0), index(row, 0));

        if (state == MegaTransfer::STATE_FAILED)
        {
            mFailedTransfers[tag] = transfer->copy();
        }

        // Keep statistics up to date
        if (prevState != state)
        {
            mNbTransfersPerState[prevState]--;
            mNbTransfersPerState[state]++;

            emit nbOfTransfersPerStateChanged(prevState, mNbTransfersPerState[prevState]);
            emit nbOfTransfersPerStateChanged(state, mNbTransfersPerState[state]);

            if (transfer->isFinished())
            {
                auto type (transferItem->getType());
                mNbTransfersPerType[type]--;
                emit nbOfTransfersPerTypeChanged(type, mNbTransfersPerType[type]);
            }
        }

        auto rem (mRemainingTimes.take(tag));
        if (rem != nullptr)
        {
            delete rem;
        }
    }
    mNotificationNumber = transfer->getNotificationNumber();
}

void QTransfersModel2::onTransferUpdate(mega::MegaApi *api, mega::MegaTransfer *transfer)
{
    if (transfer->isStreamingTransfer()
            || transfer->isFolderTransfer()
            || mNotificationNumber >= transfer->getNotificationNumber())
    {
        return;
    }

    TransferTag tag (transfer->getTag());
    auto row (mOrder.indexOf(tag));

    if (row >= 0)
    {
        auto transferItem (static_cast<TransferItem2*>(mTransfers[tag].data()));
        auto d(transferItem->getTransferData());

        auto speed (transfer->getSpeed());
        auto totalBytes (transfer->getTotalBytes());
        auto transferredBytes(transfer->getTransferredBytes());
        TransferRemainingTime* rem (mRemainingTimes[transfer->getTag()]);
        auto remSecs (rem->calculateRemainingTimeSeconds(speed, totalBytes-transferredBytes));

        auto state (transfer->getState());
        auto prevState = transferItem->getState();

        auto priority (transfer->getPriority());
        auto prevPriority (d->mPriority);

        transferItem->updateValuesTransferUpdated(transfer->getUpdateTime(),
                                        remSecs.count(),
                                        0,
                                        0,
                                        transfer->getMeanSpeed(),
                                        speed,
                                        priority,
                                        state,
                                        transferredBytes);

        bool rowMoved(false);
        if (priority != prevPriority)
        {
            // Get neighbours
            QExplicitlySharedDataPointer<TransferData> dOther;
            auto newRow (row);
            TransferTag tagOther;

            mModelMutex.lock();
            int lastRow(mOrder.size()-1);
            int rowToPassToBeginMove(0);

            if (priority > prevPriority && row < lastRow)
            {
                do
                {
                    newRow++;
                    tagOther = mOrder.at(newRow);
                    dOther = static_cast<TransferItem2*>(mTransfers[tagOther].data())->getTransferData();
                }
                while (priority > dOther->mPriority && newRow < lastRow);
                if (priority < dOther->mPriority)
                {
                    newRow--;
                }
                rowToPassToBeginMove = newRow+1;
            }
            else if (priority < prevPriority && row > 0)
            {
                do
                {
                    newRow--;
                    tagOther = mOrder.at(newRow);
                    dOther = static_cast<TransferItem2*>(mTransfers[tagOther].data())->getTransferData();
                }
                while (priority < dOther->mPriority && newRow > 0);
                if (priority > dOther->mPriority)
                {
                    newRow++;
                }
                rowToPassToBeginMove = newRow;
            }

            if (newRow != row)
            {
                if (beginMoveRows(QModelIndex(), row, 1, QModelIndex(), rowToPassToBeginMove))
                {
                    mOrder.move(row, newRow);
                    endMoveRows();
                    rowMoved = true;
                }
            }
            mModelMutex.unlock();
        }

        if (!rowMoved)
        {
            emit dataChanged(index(row, 0), index(row, 0));
        }

        // Keep statistics up to date
        if (prevState != state)
        {
            mNbTransfersPerState[prevState]--;
            mNbTransfersPerState[state]++;

            emit nbOfTransfersPerStateChanged(prevState, mNbTransfersPerState[prevState]);
            emit nbOfTransfersPerStateChanged(state, mNbTransfersPerState[state]);

            if (transfer->isFinished())
            {
                auto type (transferItem->getType());
                auto fileType (transferItem->getFileType());

                mNbTransfersPerType[type]--;
                mNbTransfersPerFileType[fileType]--;

                emit nbOfTransfersPerTypeChanged(type, mNbTransfersPerType[type]);
                emit nbOfTransfersPerFileTypeChanged(fileType, mNbTransfersPerFileType[fileType]);
            }
        }
    }
    mNotificationNumber = transfer->getNotificationNumber();
}

void QTransfersModel2::onTransferTemporaryError(mega::MegaApi *api,mega::MegaTransfer *transfer, mega::MegaError* error)
{
    if (transfer->isStreamingTransfer()
            || transfer->isFolderTransfer()
            || mNotificationNumber >= transfer->getNotificationNumber()
            || !transfer->getPriority())
    {
        return;
    }

    TransferTag tag (transfer->getTag());
    auto row (mOrder.indexOf(tag));

    if (row >= 0)
    {
        auto transferItem (static_cast<TransferItem2*>(mTransfers[tag].data()));

        auto speed (transfer->getSpeed());
        auto totalBytes (transfer->getTotalBytes());
        auto transferredBytes(transfer->getTransferredBytes());
        TransferRemainingTime* rem (mRemainingTimes[transfer->getTag()]);
        auto remSecs (rem->calculateRemainingTimeSeconds(speed, totalBytes-transferredBytes));

        int errorCode(MegaError::API_OK);
        auto errorValue(0LL);
        auto megaError (transfer->getLastErrorExtended());
        if (megaError != nullptr)
        {
            errorCode = megaError->getErrorCode();
            errorValue = megaError->getValue();
        }

        auto state (transfer->getState());
        auto prevState = transferItem->getState();

        transferItem->updateValuesTransferUpdated(transfer->getUpdateTime(),
                                                  remSecs.count(),
                                                  errorCode,
                                                  errorValue,
                                                  transfer->getMeanSpeed(),
                                                  speed,
                                                  transfer->getPriority(),
                                                  state,
                                                  transferredBytes);

        emit dataChanged(index(row, 0), index(row, 0));

        // Keep statistics up to date
        if (prevState != state)
        {
            mNbTransfersPerState[prevState]--;
            mNbTransfersPerState[state]++;

            emit nbOfTransfersPerStateChanged(prevState, mNbTransfersPerState[prevState]);
            emit nbOfTransfersPerStateChanged(state, mNbTransfersPerState[state]);

            if (transfer->isFinished())
            {
                auto type (transferItem->getType());
                auto fileType (transferItem->getFileType());

                mNbTransfersPerType[type]--;
                mNbTransfersPerFileType[fileType]--;

                emit nbOfTransfersPerTypeChanged(type, mNbTransfersPerType[type]);
                emit nbOfTransfersPerFileTypeChanged(fileType, mNbTransfersPerFileType[fileType]);
            }
        }
    }
    mNotificationNumber = transfer->getNotificationNumber();
}

bool QTransfersModel2::areDlPaused()
{
    return mAreDlPaused;
}

bool QTransfersModel2::areUlPaused()
{
    return mAreUlPaused;
}

void QTransfersModel2::onPauseStateChanged()
{
    mAreDlPaused = mPreferences->getDownloadsPaused();
    mAreUlPaused = mPreferences->getUploadsPaused();

    emit dataChanged(index(0, 0), index(mOrder.size()-1, 0), {Qt::DisplayRole});
}

void QTransfersModel2::onRetryTransfer(TransferTag tag)
{
    auto transfer(mFailedTransfers[tag]);
    if (transfer)
    {
        TransferItem2 transferItem (qvariant_cast<TransferItem2>(mTransfers[tag]));
        transferItem.getTransferData()->mMegaApi->retryTransfer(transfer);

        removeRows(mOrder.indexOf(tag), 1, QModelIndex());
    }
}

bool QTransfersModel2::removeRows(int row, int count, const QModelIndex &parent)
{
    if (parent == QModelIndex() && count > 0 && row >= 0)
    {
        mModelMutex.lock();

        beginRemoveRows(QModelIndex(), row, row + count - 1);

        for (auto i(0); i < count; ++i)
        {
            TransferTag tag (mOrder.takeAt(row));
            TransferItem2 transferItem (qvariant_cast<TransferItem2>(mTransfers.take(tag)));

            // Keep statistics updated
            auto state(transferItem.getState());
            auto fileType(transferItem.getFileType());

            mNbTransfersPerState[state]--;
            mNbTransfersPerFileType[fileType]--;

            emit nbOfTransfersPerStateChanged(state, mNbTransfersPerState[state]);
            emit nbOfTransfersPerFileTypeChanged(fileType, mNbTransfersPerFileType[fileType]);

            if (!(state == MegaTransfer::STATE_COMPLETED
                    || state == MegaTransfer::STATE_CANCELLED
                    || state == MegaTransfer::STATE_FAILED))
            {
                auto type(transferItem.getType());
                mNbTransfersPerType[type]--;
                emit nbOfTransfersPerTypeChanged(type, mNbTransfersPerType[type]);
            }

            if (state == MegaTransfer::STATE_FAILED)
            {
                auto transfer(mFailedTransfers.take(tag));
                if (transfer)
                {
                    delete transfer;
                }
            }

            auto rem (mRemainingTimes.take(tag));
            if (rem != nullptr)
            {
                delete rem;
            }
        }

        endRemoveRows();

        mModelMutex.unlock();
        return true;
    }
    else
    {
        return false;
    }
}

bool QTransfersModel2::moveRows(const QModelIndex &sourceParent, int sourceRow, int count,
                                const QModelIndex &destinationParent, int destinationChild)
{
    int lastRow = sourceRow + count - 1;
    const int rowCount = mOrder.size();

    if (sourceParent == destinationParent
            && (destinationChild < sourceRow
                || destinationChild > lastRow))
    {
        beginMoveRows(sourceParent, sourceRow, lastRow,
                      destinationParent, destinationChild);

        mModelMutex.lock();
        // To keep order, do from first to last if destination is before first,
        // and from last to first if destination is after last.
        bool ascending (destinationChild < sourceRow ?
                    false
                  : true);
        int rowToMove = (ascending ? sourceRow : lastRow);

        QList<TransferTag> tagsToMove;

        for (auto row(sourceRow); row <= lastRow; ++row)
        {
            if (ascending)
            {
                tagsToMove.push_back(mOrder.at(row));
            }
            else
            {
                tagsToMove.push_front(mOrder.at(row));
            }
        }

        for (auto tag : tagsToMove)
        {
            auto d (qvariant_cast<TransferItem2>(mTransfers[tag]).getTransferData());
            if (destinationChild == 0)
            {
                d->mMegaApi->moveTransferToFirstByTag(d->mTag);
            }
            else if (destinationChild == rowCount)
            {
                d->mMegaApi->moveTransferToLastByTag(d->mTag);
            }
            else if (destinationChild == lastRow+1)
            {
                d->mMegaApi->moveTransferUpByTag(d->mTag);
            }
            else if (destinationChild == sourceRow-1)
            {
                d->mMegaApi->moveTransferDownByTag(d->mTag);
            }
            mOrder.move(rowToMove, destinationChild);
        }
        mModelMutex.unlock();
        endMoveRows();
        return true;
    }
    return false;
}

Qt::ItemFlags QTransfersModel2::flags(const QModelIndex &index) const
{
    Qt::ItemFlags defaultFlags = QAbstractItemModel::flags(index);
    if (index.isValid())
    {
        return Qt::ItemIsDragEnabled | defaultFlags;
    }
    return Qt::ItemIsDropEnabled | defaultFlags;
}

Qt::DropActions QTransfersModel2::supportedDropActions() const
{
    return Qt::MoveAction;
}

void QTransfersModel2::insertTransfer(mega::MegaApi *api, mega::MegaTransfer *transfer, int row)
{
    TransferTag tag (transfer->getTag());
    int state (transfer->getState());
    int type (transfer->getType());
    QString fileName (QString::fromUtf8(transfer->getFileName()));
    TransferData::FileTypes fileType = mFileTypes[Utilities::getExtensionPixmapName(fileName, QString())];
    auto speed (api->getCurrentSpeed(type));
    auto totalBytes (transfer->getTotalBytes());
    auto transferredBytes(transfer->getTransferredBytes());
    TransferRemainingTime* rem (new TransferRemainingTime(speed, totalBytes-transferredBytes));
    auto remSecs (rem->calculateRemainingTimeSeconds(speed, totalBytes-transferredBytes));
    auto priority = transfer->getPriority();
    int errorCode(MegaError::API_OK);
    auto errorValue(0LL);
    auto megaError (transfer->getLastErrorExtended());
    if (megaError != nullptr)
    {
        errorCode = megaError->getErrorCode();
        errorValue = megaError->getValue();
    }
    bool isPublic (transfer->getPublicMegaNode());

    TransferData dataRow(
                type,
                errorCode,
                state,
                tag,
                errorValue,
                0,
                remSecs.count(),
                totalBytes,
                priority,
                speed,
                transfer->getMeanSpeed(),
                transferredBytes,
                transfer->getUpdateTime(),
                isPublic,
                transfer->isSyncTransfer(),
                fileType,
                api,
                fileName);

     mTransfers[tag] = QVariant::fromValue(TransferItem2(dataRow));
     mRemainingTimes[tag] = rem;

//     // Find place in order
//     if (priority < qvariant_cast<TransferItem2>(mTransfers[mOrder.first()]).getTransferData()->mPriority)
//     {
//        mOrder.push_front(tag);
//     }
//     else if (priority < qvariant_cast<TransferItem2>(mTransfers[mOrder.last()]).getTransferData()->mPriority)
//     {
        mOrder.insert(row, tag);
//     }
//     else
//     {

//     }


     // Update statistics
     mNbTransfersPerState[state]++;
     mNbTransfersPerFileType[fileType]++;
     mNbTransfersPerType[type]++;
}

void QTransfersModel2::emitStatistics()
{
    for (auto state : mNbTransfersPerState.keys())
    {
            emit nbOfTransfersPerStateChanged(state, mNbTransfersPerState[state]);
    }

    for (auto type : mNbTransfersPerType.keys())
    {
            emit nbOfTransfersPerTypeChanged(type, mNbTransfersPerType[type]);
    }

    for (auto fileType : mNbTransfersPerFileType.keys())
    {
            emit nbOfTransfersPerFileTypeChanged(fileType, mNbTransfersPerFileType[fileType]);
    }
}
