#ifndef SIDECHAINWITHDRAWALTABLEMODEL_H
#define SIDECHAINWITHDRAWALTABLEMODEL_H

#include <QAbstractTableModel>
#include <QList>

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

struct SidechainWithdrawalTableObject
{
    QString sidechain;
    QString hash;
    uint16_t nAcks;
    uint32_t nAge;
    uint32_t nMaxAge;
    bool fApproved;
};

class SidechainWithdrawalTableModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    explicit SidechainWithdrawalTableModel(QObject *parent = 0);
    int rowCount(const QModelIndex &parent = QModelIndex()) const;
    int columnCount(const QModelIndex &parent = QModelIndex()) const;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const;

    enum RoleIndex {
        AcksRole = Qt::UserRole,
        HashRole
    };

    // Populate the model with demo data
    void AddDemoData();

    // Clear demo data and start syncing with real data again
    void ClearDemoData();

public Q_SLOTS:
    void updateModel();

    void numBlocksChanged();

private:
    QList<QVariant> model;
};

#endif // SIDECHAINWITHDRAWALTABLEMODEL_H
