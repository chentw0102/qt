#include "particlesystem.h"
#include <node.h>
#include "particleemitter.h"
#include "particleaffector.h"
#include "particle.h"
#include <cmath>
#include <QDebug>

ParticleData::ParticleData()
    : group(0)
    , e(0)
    , particleIndex(0)
    , systemIndex(0)
{
    pv.x = 0;
    pv.y = 0;
    pv.t = -1;
    pv.size = 0;
    pv.endSize = 0;
    pv.sx = 0;
    pv.sy = 0;
    pv.ax = 0;
    pv.ay = 0;
}

ParticleSystem::ParticleSystem(QSGItem *parent) :
    QSGItem(parent), m_running(true) , m_startTime(0)
{
    m_groupIds = QHash<QString, int>();
}

void ParticleSystem::registerParticleType(ParticleType* p)
{
    m_particles << p;//###Set or uniqueness checking?
    reset();
}

void ParticleSystem::registerParticleEmitter(ParticleEmitter* e)
{
    m_emitters << e;//###How to get them out?
    connect(e, SIGNAL(particlesPerSecondChanged(qreal)),
            this, SLOT(countChanged()));
    connect(e, SIGNAL(particleDurationChanged(int)),
            this, SLOT(countChanged()));
    reset();
}

void ParticleSystem::registerParticleAffector(ParticleAffector* a)
{
    m_affectors << a;
    //reset();//TODO: Slim down the huge batch of resets at the start
}

void ParticleSystem::countChanged()
{
    reset();//Need to give Particles new Count
}

void ParticleSystem::setRunning(bool arg)
{
    if (m_running != arg) {
        m_running = arg;
        emit runningChanged(arg);
        reset();
    }
}

void ParticleSystem::componentComplete()
{
    QSGItem::componentComplete();
    reset();
}

void ParticleSystem::initializeSystem()
{
    m_particle_count = 0;//TODO: Only when changed?

    for(QHash<int, GroupData*>::iterator iter = m_groupData.begin(); iter != m_groupData.end(); iter++)
        delete (*iter);
    m_groupData.clear();
    m_groupIds.clear();

    GroupData* gd = new GroupData;//Default group
    gd->size = 0;
    gd->start = -1;
    gd->nextIdx = 0;
    m_groupData.insert(0,gd);
    m_groupIds.insert("",0);
    m_nextGroupId = 1;

    if(!m_emitters.count() || !m_particles.count())
        return;

    foreach(ParticleEmitter* e, m_emitters){
        if(!m_groupIds.contains(e->particle())
                || (!e->particle().isEmpty() && !m_groupIds[e->particle()])){//or it was accidentally inserted by a failed lookup earlier
            GroupData* gd = new GroupData;
            gd->size = 0;
            gd->start = -1;
            gd->nextIdx = 0;
            int id = m_nextGroupId++;
            m_groupIds.insert(e->particle(), id);
            m_groupData.insert(id, gd);
        }
        m_groupData[m_groupIds[e->particle()]]->size += ceil(e->particlesPerSecond()*((e->particleDuration()+e->particleDurationVariation())/1000.0));
    }

    for(QHash<int, GroupData*>::iterator iter = m_groupData.begin(); iter != m_groupData.end(); iter++){
        (*iter)->start = m_particle_count;
        m_particle_count += (*iter)->size;
    }
        m_data.resize(m_particle_count);
    if(m_particle_count > 16000)
        qWarning() << "Particle system contains a vast number of particles (>16000). Expect poor performance";

    foreach(ParticleType* particle, m_particles){
        int particleCount = 0;
        if(particle->particles().isEmpty()){//Uses default particle
            particleCount += m_groupData[0]->size;
            m_groupData[0]->types << particle;
        }else{
            foreach(const QString &group, particle->particles()){
                particleCount += m_groupData[m_groupIds[group]]->size;
                m_groupData[m_groupIds[group]]->types << particle;
            }
        }
        particle->setCount(particleCount);
        particle->m_pleaseReset = true;
    }

    m_timestamp.start();
    m_initialized = true;
    emit systemInitialized();
}

void ParticleSystem::reset()
{
    foreach(ParticleType* p, m_particles)
        p->reset();
    foreach(ParticleEmitter* e, m_emitters)
        e->reset();
    if(!m_running)
        return;
    initializeSystem();
    foreach(ParticleType* p, m_particles)
        p->update();
}

ParticleData* ParticleSystem::newDatum(int groupId)
{
    int nextIdx = m_groupData[groupId]->start + m_groupData[groupId]->nextIdx++;
    if( m_groupData[groupId]->nextIdx >= m_groupData[groupId]->size)
        m_groupData[groupId]->nextIdx = 0;

    ParticleData* ret;
    if(m_data[nextIdx]){//Recycle, it's faster. //###Reset?
        ret = m_data[nextIdx];
    }else{
        ret = new ParticleData;
        m_data[nextIdx] = ret;
    }

    ret->system = this;
    ret->systemIndex = nextIdx;
    ret->particleIndex = nextIdx - m_groupData[groupId]->start;
    ret->group = groupId;
    return ret;
}

void ParticleSystem::emitParticle(ParticleData* pd)
{// called from prepareNextFrame()->emitWindow - enforce?
    //Account for relative emitter position
    QPointF offset = this->mapFromItem(pd->e, QPointF(0, 0));
    if(!offset.isNull()){
        pd->pv.x += offset.x();
        pd->pv.y += offset.y();
    }

    foreach(ParticleAffector *a, m_affectors)
        if(a->m_needsReset)
            a->reset(pd->systemIndex);
    foreach(ParticleType* p, m_groupData[pd->group]->types)
        p->load(pd);
}



uint ParticleSystem::systemSync(ParticleType* p)
{
    if (!m_running)
        return 0;
    if (!m_initialized)
        return 0;//error in initialization

    if(m_syncList.isEmpty() || m_syncList.contains(p)){//Need to advance the simulation
        m_syncList.clear();

        //### Elapsed time never shrinks - may cause problems if left emitting for weeks at a time.
        qreal dt = m_timeInt / 1000.;
        m_timeInt = m_timestamp.elapsed() + m_startTime;
        qreal time =  m_timeInt / 1000.;
        dt = time - dt;
        foreach(ParticleEmitter* emitter, m_emitters)
            emitter->emitWindow(m_timeInt);
        foreach(ParticleAffector* a, m_affectors)
            a->affectSystem(dt);
        foreach(ParticleData* d, m_needsReset)
            foreach(ParticleType* p, m_groupData[d->group]->types)
                p->reload(d);
    }
    m_syncList << p;
    return m_timeInt;
}

//sets the x accleration without affecting the instantaneous x velocity or position
void ParticleData::setInstantaneousAX(qreal ax)
{
    qreal t = (system->m_timeInt / 1000.0) - pv.t;
    qreal sx = (pv.sx + t*pv.ax) - t*ax;
    qreal ex = pv.x + pv.sx * t + 0.5 * pv.ax * t * t;
    qreal x = ex - t*sx - 0.5 * t*t*ax;

    pv.ax = ax;
    pv.sx = sx;
    pv.x = x;
}

//sets the x velocity without affecting the instantaneous x postion
void ParticleData::setInstantaneousSX(qreal vx)
{
    qreal t = (system->m_timeInt / 1000.0) - pv.t;
    qreal sx = vx - t*pv.ax;
    qreal ex = pv.x + pv.sx * t + 0.5 * pv.ax * t * t;
    qreal x = ex - t*sx - 0.5 * t*t*pv.ax;

    pv.sx = sx;
    pv.x = x;
}

//sets the instantaneous x postion
void ParticleData::setInstantaneousX(qreal x)
{
    qreal t = (system->m_timeInt / 1000.0) - pv.t;
    pv.x = x - t*pv.sx - 0.5 * t*t*pv.ax;
}

//sets the y accleration without affecting the instantaneous y velocity or position
void ParticleData::setInstantaneousAY(qreal ay)
{
    qreal t = (system->m_timeInt / 1000.0) - pv.t;
    qreal sy = (pv.sy + t*pv.ay) - t*ay;
    qreal ey = pv.y + pv.sy * t + 0.5 * pv.ay * t * t;
    qreal y = ey - t*sy - 0.5 * t*t*ay;

    pv.ay = ay;
    pv.sy = sy;
    pv.y = y;
}

//sets the y velocity without affecting the instantaneous y position
void ParticleData::setInstantaneousSY(qreal vy)
{
    qreal t = (system->m_timeInt / 1000.0) - pv.t;
    //qDebug() << t << (system->m_timeInt/1000.0) << pv.x << pv.sx << pv.ax << pv.x + pv.sx * t + 0.5 * pv.ax * t * t;
    qreal sy = vy - t*pv.ay;
    qreal ey = pv.y + pv.sy * t + 0.5 * pv.ay * t * t;
    qreal y = ey - t*sy - 0.5 * t*t*pv.ay;

    pv.sy = sy;
    pv.y = y;
}

//sets the instantaneous Y position
void ParticleData::setInstantaneousY(qreal y)
{
    qreal t = (system->m_timeInt / 1000.0) - pv.t;
    pv.y = y - t*pv.sy - 0.5 * t*t*pv.ay;
}

qreal ParticleData::curX() const
{
    qreal t = (system->m_timeInt / 1000.0) - pv.t;
    return pv.x + pv.sx * t + 0.5 * pv.ax * t * t;
}

qreal ParticleData::curSX() const
{
    qreal t = (system->m_timeInt / 1000.0) - pv.t;
    return pv.sx + t*pv.ax;
}

qreal ParticleData::curY() const
{
    qreal t = (system->m_timeInt / 1000.0) - pv.t;
    return pv.y + pv.sy * t + 0.5 * pv.ay * t * t;
}

qreal ParticleData::curSY() const
{
    qreal t = (system->m_timeInt / 1000.0) - pv.t;
    return pv.sy + t*pv.ay;
}

void ParticleData::debugDump()
{
    qDebug() << "Particle" << group
             << "Pos: " << pv.x << "," << pv.y
             << "Vel: " << pv.sx << "," << pv.sy
             << "Acc: " << pv.ax << "," << pv.ay
             << "Time: " << pv.t << "," << (system->m_timeInt / 1000.0);
}