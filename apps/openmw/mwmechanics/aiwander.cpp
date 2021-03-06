#include "aiwander.hpp"

#include <OgreVector3.h>
#include <OgreSceneNode.h>

#include <components/esm/aisequence.hpp>

#include "../mwbase/world.hpp"
#include "../mwbase/environment.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/dialoguemanager.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/cellstore.hpp"

#include "creaturestats.hpp"
#include "steering.hpp"
#include "movement.hpp"



namespace MWMechanics
{
    static const int COUNT_BEFORE_RESET = 200; // TODO: maybe no longer needed
    static const float DOOR_CHECK_INTERVAL = 1.5f;
    static const float REACTION_INTERVAL = 0.25f;
    static const int GREETING_SHOULD_START = 4; //how many reaction intervals should pass before NPC can greet player
    static const int GREETING_SHOULD_END = 10;

    /// \brief This class holds the variables AiWander needs which are deleted if the package becomes inactive.
    struct AiWanderStorage : AiTemporaryBase
    {
        // the z rotation angle (degrees) we want to reach
        // used every frame when mRotate is true
        Ogre::Radian mTargetAngle;
        bool mRotate;
        float mReaction; // update some actions infrequently
        
        
        AiWander::GreetingState mSaidGreeting;
        int mGreetingTimer;
        
        // Cached current cell location
        int mCellX;
        int mCellY;
        // Cell location multiplied by ESM::Land::REAL_SIZE
        float mXCell;
        float mYCell;
        
        const MWWorld::CellStore* mCell; // for detecting cell change
        
        // AiWander states
        bool mChooseAction;
        bool mIdleNow;
        bool mMoveNow;
        bool mWalking;
        
        unsigned short mPlayedIdle;

        PathFinder mPathFinder;
        
        AiWanderStorage():
            mTargetAngle(0),
            mRotate(false),
            mReaction(0),
            mSaidGreeting(AiWander::Greet_None),
            mGreetingTimer(0),
            mCellX(std::numeric_limits<int>::max()),
            mCellY(std::numeric_limits<int>::max()),
            mXCell(0),
            mYCell(0),
            mCell(NULL),
            mChooseAction(true),
            mIdleNow(false),
            mMoveNow(false),
            mWalking(false),
            mPlayedIdle(0)
            {};
    };
    
    AiWander::AiWander(int distance, int duration, int timeOfDay, const std::vector<unsigned char>& idle, bool repeat):
        mDistance(distance), mDuration(duration), mTimeOfDay(timeOfDay), mIdle(idle), mRepeat(repeat)
    {
        mIdle.resize(8, 0);
        init();
    }

    void AiWander::init()
    {
        // NOTE: mDistance and mDuration must be set already


        mStuckCount = 0;// TODO: maybe no longer needed
        mDoorCheckDuration = 0;
        mTrimCurrentNode = false;

        mHasReturnPosition = false;
        mReturnPosition = Ogre::Vector3(0,0,0);

        if(mDistance < 0)
            mDistance = 0;
        if(mDuration < 0)
            mDuration = 0;
        if(mDuration == 0)
            mTimeOfDay = 0;

        mStartTime = MWBase::Environment::get().getWorld()->getTimeStamp();

        mStoredAvailableNodes = false;

    }

    AiPackage * MWMechanics::AiWander::clone() const
    {
        return new AiWander(*this);
    }

    /*
     * AiWander high level states (0.29.0). Not entirely accurate in some cases
     * e.g. non-NPC actors do not greet and some creatures may be moving even in
     * the IdleNow state.
     *
     *                          [select node,
     *                           build path]
     *                 +---------->MoveNow----------->Walking
     *                 |                                 |
     * [allowed        |                                 |
     *  nodes]         |        [hello if near]          |
     *  start--->ChooseAction----->IdleNow               |
     *                ^ ^           |                    |
     *                | |           |                    |
     *                | +-----------+                    |
     *                |                                  |
     *                +----------------------------------+
     *
     *
     * New high level states.  Not exactly as per vanilla (e.g. door stuff)
     * but the differences are required because our physics does not work like
     * vanilla and therefore have to compensate/work around.
     *
     *                         [select node,     [if stuck evade
     *                          build path]       or remove nodes if near door]
     *                 +---------->MoveNow<---------->Walking
     *                 |              ^                | |
     *                 |              |(near door)     | |
     * [allowed        |              |                | |
     *  nodes]         |        [hello if near]        | |
     *  start--->ChooseAction----->IdleNow             | |
     *                ^ ^           |  ^               | |
     *                | |           |  | (stuck near   | |
     *                | +-----------+  +---------------+ |
     *                |                    player)       |
     *                +----------------------------------+
     *
     * NOTE: non-time critical operations are run once every 250ms or so.
     *
     * TODO: It would be great if door opening/closing can be detected and pathgrid
     * links dynamically updated.  Currently (0.29.0) AiWander allows choosing a
     * destination beyond closed doors which sometimes makes the actors stuck at the
     * door and impossible for the player to open the door.
     *
     * For now detect being stuck at the door and simply delete the nodes from the
     * allowed set.  The issue is when the door opens the allowed set is not
     * re-calculated.  However this would not be an issue in most cases since hostile
     * actors will enter combat (i.e. no longer wandering) and different pathfinding
     * will kick in.
     */
    bool AiWander::execute (const MWWorld::Ptr& actor, AiState& state, float duration)
    {
        // get or create temporary storage
        AiWanderStorage& storage = state.get<AiWanderStorage>();
        

        const MWWorld::CellStore*& currentCell = storage.mCell;
        MWMechanics::CreatureStats& cStats = actor.getClass().getCreatureStats(actor);
        if(cStats.isDead() || cStats.getHealth().getCurrent() <= 0)
            return true; // Don't bother with dead actors

        bool cellChange = currentCell && (actor.getCell() != currentCell);
        if(!currentCell || cellChange)
        {
            currentCell = actor.getCell();
            mStoredAvailableNodes = false; // prob. not needed since mDistance = 0
        }
        const ESM::Cell *cell = currentCell->getCell();

        cStats.setDrawState(DrawState_Nothing);
        cStats.setMovementFlag(CreatureStats::Flag_Run, false);

        ESM::Position pos = actor.getRefData().getPosition();
        
        
        bool& idleNow = storage.mIdleNow;
        bool& moveNow = storage.mMoveNow;
        bool& walking = storage.mWalking;
        // Check if an idle actor is  too close to a door - if so start walking
        mDoorCheckDuration += duration;
        if(mDoorCheckDuration >= DOOR_CHECK_INTERVAL)
        {
            mDoorCheckDuration = 0;    // restart timer
            if(mDistance &&            // actor is not intended to be stationary
                idleNow &&             // but is in idle
               !walking &&            // FIXME: some actors are idle while walking
               proximityToDoor(actor, MIN_DIST_TO_DOOR_SQUARED*1.6*1.6)) // NOTE: checks interior cells only
            {
                idleNow = false;
                moveNow = true;
                mTrimCurrentNode = false; // just in case
            }
        }

        // Are we there yet?
        bool& chooseAction = storage.mChooseAction;
        if(walking &&
           storage.mPathFinder.checkPathCompleted(pos.pos[0], pos.pos[1], pos.pos[2]))
        {
            stopWalking(actor, storage);
            moveNow = false;
            walking = false;
            chooseAction = true;
            mHasReturnPosition = false;
        }


        
        if(walking) // have not yet reached the destination
        {
            // turn towards the next point in mPath
            zTurn(actor, Ogre::Degree(storage.mPathFinder.getZAngleToNext(pos.pos[0], pos.pos[1])));
            actor.getClass().getMovementSettings(actor).mPosition[1] = 1;

            // Returns true if evasive action needs to be taken
            if(mObstacleCheck.check(actor, duration))
            {
                // first check if we're walking into a door
                if(proximityToDoor(actor)) // NOTE: checks interior cells only
                {
                    // remove allowed points then select another random destination
                    mTrimCurrentNode = true;
                    trimAllowedNodes(mAllowedNodes, storage.mPathFinder);
                    mObstacleCheck.clear();
                    storage.mPathFinder.clearPath();
                    walking = false;
                    moveNow = true;
                }
                else // probably walking into another NPC
                {
                    // TODO: diagonal should have same animation as walk forward
                    //       but doesn't seem to do that?
                    actor.getClass().getMovementSettings(actor).mPosition[0] = 1;
                    actor.getClass().getMovementSettings(actor).mPosition[1] = 0.1f;
                    // change the angle a bit, too
                    zTurn(actor, Ogre::Degree(storage.mPathFinder.getZAngleToNext(pos.pos[0] + 1, pos.pos[1])));
                }
                mStuckCount++;  // TODO: maybe no longer needed
            }
//#if 0
            // TODO: maybe no longer needed
            if(mStuckCount >= COUNT_BEFORE_RESET) // something has gone wrong, reset
            {
                //std::cout << "Reset \""<< cls.getName(actor) << "\"" << std::endl;
                mObstacleCheck.clear();

                stopWalking(actor, storage);
                moveNow = false;
                walking = false;
                chooseAction = true;
            }
//#endif
        }
        
        
        Ogre::Radian& targetAngle = storage.mTargetAngle;
        bool& rotate = storage.mRotate;
        if (rotate)
        {
            // Reduce the turning animation glitch by using a *HUGE* value of
            // epsilon...  TODO: a proper fix might be in either the physics or the
            // animation subsystem
            if (zTurn(actor, targetAngle, Ogre::Degree(5)))
                rotate = false;
        }

        // Check if idle animation finished
        short unsigned& playedIdle = storage.mPlayedIdle;
        GreetingState& greetingState = storage.mSaidGreeting;
        if(idleNow && !checkIdle(actor, playedIdle) && (greetingState == Greet_Done || greetingState == Greet_None))
        {
            playedIdle = 0;
            idleNow = false;
            chooseAction = true;
        }

        MWBase::World *world = MWBase::Environment::get().getWorld();

        if(chooseAction)
        {
            playedIdle = 0;
            getRandomIdle(playedIdle); // NOTE: sets mPlayedIdle with a random selection

            if(!playedIdle && mDistance)
            {
                chooseAction = false;
                moveNow = true;
            }
            else
            {
                // Play idle animation and recreate vanilla (broken?) behavior of resetting start time of AIWander:
                MWWorld::TimeStamp currentTime = world->getTimeStamp();
                mStartTime = currentTime;
                playIdle(actor, playedIdle);
                chooseAction = false;
                idleNow = true;

                // Play idle voiced dialogue entries randomly
                int hello = cStats.getAiSetting(CreatureStats::AI_Hello).getModified();
                if (hello > 0)
                {
                    int roll = std::rand()/ (static_cast<double> (RAND_MAX) + 1) * 100; // [0, 99]
                    MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();

                    // Don't bother if the player is out of hearing range
                    static float fVoiceIdleOdds = MWBase::Environment::get().getWorld()->getStore()
                            .get<ESM::GameSetting>().find("fVoiceIdleOdds")->getFloat();

                    // Only say Idle voices when player is in LOS
                    // A bit counterintuitive, likely vanilla did this to reduce the appearance of
                    // voices going through walls?
                    if (roll < fVoiceIdleOdds && Ogre::Vector3(player.getRefData().getPosition().pos).squaredDistance(Ogre::Vector3(pos.pos)) < 1500*1500
                            && MWBase::Environment::get().getWorld()->getLOS(player, actor))
                        MWBase::Environment::get().getDialogueManager()->say(actor, "idle");
                }
            }
        }

        float& lastReaction = storage.mReaction;
        lastReaction += duration;
        if(lastReaction < REACTION_INTERVAL)
        {
            return false;
        }
        else
            lastReaction = 0;

        // NOTE: everything below get updated every REACTION_INTERVAL seconds

        if(mDuration)
        {
            // End package if duration is complete or mid-night hits:
            MWWorld::TimeStamp currentTime = world->getTimeStamp();
            if(currentTime.getHour() >= mStartTime.getHour() + mDuration)
            {
                if(!mRepeat)
                {
                    stopWalking(actor, storage);
                    return true;
                }
                else
                    mStartTime = currentTime;
            }
            else if(int(currentTime.getHour()) == 0 && currentTime.getDay() != mStartTime.getDay())
            {
                if(!mRepeat)
                {
                    stopWalking(actor, storage);
                    return true;
                }
                else
                    mStartTime = currentTime;
            }
        }


        
        int& cachedCellX = storage.mCellX;
        int& cachedCellY = storage.mCellY;
        float& cachedCellXposition = storage.mXCell;
        float& cachedCellYposition = storage.mYCell;
        // Initialization to discover & store allowed node points for this actor.
        if(!mStoredAvailableNodes)
        {
            // infrequently used, therefore no benefit in caching it as a member
            const ESM::Pathgrid *
                pathgrid = world->getStore().get<ESM::Pathgrid>().search(*cell);

            // cache the current cell location
            cachedCellX = cell->mData.mX;
            cachedCellY = cell->mData.mY;

            // If there is no path this actor doesn't go anywhere. See:
            // https://forum.openmw.org/viewtopic.php?t=1556
            // http://www.fliggerty.com/phpBB3/viewtopic.php?f=30&t=5833
            if(!pathgrid || pathgrid->mPoints.empty())
                mDistance = 0;

            // A distance value passed into the constructor indicates how far the
            // actor can  wander from the spawn position.  AiWander assumes that
            // pathgrid points are available, and uses them to randomly select wander
            // destinations within the allowed set of pathgrid points (nodes).
            if(mDistance)
            {
                cachedCellXposition = 0;
                cachedCellYposition = 0;
                if(cell->isExterior())
                {
                    cachedCellXposition = cachedCellX * ESM::Land::REAL_SIZE;
                    cachedCellYposition = cachedCellY * ESM::Land::REAL_SIZE;
                }

                // FIXME: There might be a bug here.  The allowed node points are
                // based on the actor's current position rather than the actor's
                // spawn point.  As a result the allowed nodes for wander can change
                // between saves, for example.
                //
                // convert npcPos to local (i.e. cell) co-ordinates
                Ogre::Vector3 npcPos(pos.pos);
                npcPos[0] = npcPos[0] - cachedCellXposition;
                npcPos[1] = npcPos[1] - cachedCellYposition;

                // mAllowedNodes for this actor with pathgrid point indexes based on mDistance
                // NOTE: mPoints and mAllowedNodes are in local co-ordinates
                for(unsigned int counter = 0; counter < pathgrid->mPoints.size(); counter++)
                {
                    Ogre::Vector3 nodePos(pathgrid->mPoints[counter].mX, pathgrid->mPoints[counter].mY,
                        pathgrid->mPoints[counter].mZ);
                    if(npcPos.squaredDistance(nodePos) <= mDistance * mDistance)
                        mAllowedNodes.push_back(pathgrid->mPoints[counter]);
                }
                if(!mAllowedNodes.empty())
                {
                    Ogre::Vector3 firstNodePos(mAllowedNodes[0].mX, mAllowedNodes[0].mY, mAllowedNodes[0].mZ);
                    float closestNode = npcPos.squaredDistance(firstNodePos);
                    unsigned int index = 0;
                    for(unsigned int counterThree = 1; counterThree < mAllowedNodes.size(); counterThree++)
                    {
                        Ogre::Vector3 nodePos(mAllowedNodes[counterThree].mX, mAllowedNodes[counterThree].mY,
                            mAllowedNodes[counterThree].mZ);
                        float tempDist = npcPos.squaredDistance(nodePos);
                        if(tempDist < closestNode)
                            index = counterThree;
                    }
                    mCurrentNode = mAllowedNodes[index];
                    mAllowedNodes.erase(mAllowedNodes.begin() + index);

                    mStoredAvailableNodes = true; // set only if successful in finding allowed nodes
                }
            }
        }

        // Actor becomes stationary - see above URL's for previous research
        if(mAllowedNodes.empty())
            mDistance = 0;

        // Don't try to move if you are in a new cell (ie: positioncell command called) but still play idles.
        if(mDistance && cellChange)
            mDistance = 0;

        // For stationary NPCs, move back to the starting location if another AiPackage moved us elsewhere
        if (cellChange)
            mHasReturnPosition = false;
        if (mDistance == 0 && mHasReturnPosition && Ogre::Vector3(pos.pos).squaredDistance(mReturnPosition) > 20*20)
        {
            chooseAction = false;
            idleNow = false;

            if (!storage.mPathFinder.isPathConstructed())
            {
                Ogre::Vector3 destNodePos = mReturnPosition;

                ESM::Pathgrid::Point dest;
                dest.mX = destNodePos[0];
                dest.mY = destNodePos[1];
                dest.mZ = destNodePos[2];

                // actor position is already in world co-ordinates
                ESM::Pathgrid::Point start;
                start.mX = pos.pos[0];
                start.mY = pos.pos[1];
                start.mZ = pos.pos[2];

                // don't take shortcuts for wandering
                storage.mPathFinder.buildPath(start, dest, actor.getCell(), false);

                if(storage.mPathFinder.isPathConstructed())
                {
                    moveNow = false;
                    walking = true;
                }
            }
        }

        // Allow interrupting a walking actor to trigger a greeting
        if(idleNow || walking)
        {
            // Play a random voice greeting if the player gets too close
            int hello = cStats.getAiSetting(CreatureStats::AI_Hello).getModified();
            float helloDistance = hello;
            static int iGreetDistanceMultiplier =MWBase::Environment::get().getWorld()->getStore()
                .get<ESM::GameSetting>().find("iGreetDistanceMultiplier")->getInt();

            helloDistance *= iGreetDistanceMultiplier;

            MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
            Ogre::Vector3 playerPos(player.getRefData().getPosition().pos);
            Ogre::Vector3 actorPos(actor.getRefData().getPosition().pos);
            float playerDistSqr = playerPos.squaredDistance(actorPos);

            int& greetingTimer = storage.mGreetingTimer;
            if (greetingState == Greet_None)
            {
                if ((playerDistSqr <= helloDistance*helloDistance) &&
                        !player.getClass().getCreatureStats(player).isDead() && MWBase::Environment::get().getWorld()->getLOS(player, actor)
                    && MWBase::Environment::get().getMechanicsManager()->awarenessCheck(player, actor))
                    greetingTimer++;
                
                if (greetingTimer >= GREETING_SHOULD_START)
                {
                    greetingState = Greet_InProgress;
                    MWBase::Environment::get().getDialogueManager()->say(actor, "hello");
                    greetingTimer = 0;
                }
            }
            
            if(greetingState == Greet_InProgress)
            {
                greetingTimer++;
                
                if(walking)
                {
                    stopWalking(actor, storage);
                    moveNow = false;
                    walking = false;
                    mObstacleCheck.clear();
                    idleNow = true;
                    getRandomIdle(playedIdle);
                }

                if(!rotate)
                {
                    Ogre::Vector3 dir = playerPos - actorPos;

                    Ogre::Radian faceAngle = Ogre::Math::ATan2(dir.x,dir.y);
                    Ogre::Radian actorAngle = actor.getRefData().getBaseNode()->getOrientation().getRoll();
                    // an attempt at reducing the turning animation glitch
                    if( Ogre::Math::Abs( faceAngle - actorAngle ) >= Ogre::Degree(5) ) // TODO: is there a better way?
                    {
                        targetAngle = faceAngle;
                        rotate = true;
                    }
                }
                
                if (greetingTimer >= GREETING_SHOULD_END)
                {
                    greetingState = Greet_Done;
                    greetingTimer = 0;
                }
            }
            
            if (greetingState == MWMechanics::AiWander::Greet_Done)
            {
                static float fGreetDistanceReset = MWBase::Environment::get().getWorld()->getStore()
                        .get<ESM::GameSetting>().find("fGreetDistanceReset")->getFloat();

                if (playerDistSqr >= fGreetDistanceReset*fGreetDistanceReset)
                    greetingState = Greet_None;
            }
        }

        if(moveNow && mDistance)
        {
            // Construct a new path if there isn't one
            if(!storage.mPathFinder.isPathConstructed())
            {
                assert(mAllowedNodes.size());
                unsigned int randNode = (int)(rand() / ((double)RAND_MAX + 1) * mAllowedNodes.size());
                // NOTE: initially constructed with local (i.e. cell) co-ordinates
                Ogre::Vector3 destNodePos(mAllowedNodes[randNode].mX,
                                          mAllowedNodes[randNode].mY,
                                          mAllowedNodes[randNode].mZ);

                // convert dest to use world co-ordinates
                ESM::Pathgrid::Point dest;
                dest.mX = destNodePos[0] + cachedCellXposition;
                dest.mY = destNodePos[1] + cachedCellYposition;
                dest.mZ = destNodePos[2];

                // actor position is already in world co-ordinates
                ESM::Pathgrid::Point start;
                start.mX = pos.pos[0];
                start.mY = pos.pos[1];
                start.mZ = pos.pos[2];

                // don't take shortcuts for wandering
                storage.mPathFinder.buildPath(start, dest, actor.getCell(), false);

                if(storage.mPathFinder.isPathConstructed())
                {
                    // buildPath inserts dest in case it is not a pathgraph point
                    // index which is a duplicate for AiWander.  However below code
                    // does not work since getPath() returns a copy of path not a
                    // reference
                    //if(storage.mPathFinder.getPathSize() > 1)
                        //storage.mPathFinder.getPath().pop_back();

                    // Remove this node as an option and add back the previously used node (stops NPC from picking the same node):
                    ESM::Pathgrid::Point temp = mAllowedNodes[randNode];
                    mAllowedNodes.erase(mAllowedNodes.begin() + randNode);
                    // check if mCurrentNode was taken out of mAllowedNodes
                    if(mTrimCurrentNode && mAllowedNodes.size() > 1)
                        mTrimCurrentNode = false;
                    else
                        mAllowedNodes.push_back(mCurrentNode);
                    mCurrentNode = temp;

                    moveNow = false;
                    walking = true;
                }
                // Choose a different node and delete this one from possible nodes because it is uncreachable:
                else
                    mAllowedNodes.erase(mAllowedNodes.begin() + randNode);
            } 
        }

        return false; // AiWander package not yet completed
    }

    void AiWander::trimAllowedNodes(std::vector<ESM::Pathgrid::Point>& nodes,
                                    const PathFinder& pathfinder)
    {
        // TODO: how to add these back in once the door opens?
        // Idea: keep a list of detected closed doors (see aicombat.cpp)
        // Every now and then check whether one of the doors is opened. (maybe
        // at the end of playing idle?) If the door is opened then re-calculate
        // allowed nodes starting from the spawn point.
        std::list<ESM::Pathgrid::Point> paths = pathfinder.getPath();
        while(paths.size() >= 2)
        {
            ESM::Pathgrid::Point pt = paths.back();
            for(unsigned int j = 0; j < nodes.size(); j++)
            {
                // FIXME: doesn't hadle a door with the same X/Y
                //        co-ordinates but with a different Z
                if(nodes[j].mX == pt.mX && nodes[j].mY == pt.mY)
                {
                    nodes.erase(nodes.begin() + j);
                    break;
                }
            }
            paths.pop_back();
        }
    }

    int AiWander::getTypeId() const
    {
        return TypeIdWander;
    }

    void AiWander::stopWalking(const MWWorld::Ptr& actor, AiWanderStorage& storage)
    {
        storage.mPathFinder.clearPath();
        actor.getClass().getMovementSettings(actor).mPosition[1] = 0;
    }

    void AiWander::playIdle(const MWWorld::Ptr& actor, unsigned short idleSelect)
    {
        if(idleSelect == 2)
            MWBase::Environment::get().getMechanicsManager()->playAnimationGroup(actor, "idle2", 0, 1);
        else if(idleSelect == 3)
            MWBase::Environment::get().getMechanicsManager()->playAnimationGroup(actor, "idle3", 0, 1);
        else if(idleSelect == 4)
            MWBase::Environment::get().getMechanicsManager()->playAnimationGroup(actor, "idle4", 0, 1);
        else if(idleSelect == 5)
            MWBase::Environment::get().getMechanicsManager()->playAnimationGroup(actor, "idle5", 0, 1);
        else if(idleSelect == 6)
            MWBase::Environment::get().getMechanicsManager()->playAnimationGroup(actor, "idle6", 0, 1);
        else if(idleSelect == 7)
            MWBase::Environment::get().getMechanicsManager()->playAnimationGroup(actor, "idle7", 0, 1);
        else if(idleSelect == 8)
            MWBase::Environment::get().getMechanicsManager()->playAnimationGroup(actor, "idle8", 0, 1);
        else if(idleSelect == 9)
            MWBase::Environment::get().getMechanicsManager()->playAnimationGroup(actor, "idle9", 0, 1);
    }

    bool AiWander::checkIdle(const MWWorld::Ptr& actor, unsigned short idleSelect)
    {
        if(idleSelect == 2)
            return MWBase::Environment::get().getMechanicsManager()->checkAnimationPlaying(actor, "idle2");
        else if(idleSelect == 3)
            return MWBase::Environment::get().getMechanicsManager()->checkAnimationPlaying(actor, "idle3");
        else if(idleSelect == 4)
            return MWBase::Environment::get().getMechanicsManager()->checkAnimationPlaying(actor, "idle4");
        else if(idleSelect == 5)
            return MWBase::Environment::get().getMechanicsManager()->checkAnimationPlaying(actor, "idle5");
        else if(idleSelect == 6)
            return MWBase::Environment::get().getMechanicsManager()->checkAnimationPlaying(actor, "idle6");
        else if(idleSelect == 7)
            return MWBase::Environment::get().getMechanicsManager()->checkAnimationPlaying(actor, "idle7");
        else if(idleSelect == 8)
            return MWBase::Environment::get().getMechanicsManager()->checkAnimationPlaying(actor, "idle8");
        else if(idleSelect == 9)
            return MWBase::Environment::get().getMechanicsManager()->checkAnimationPlaying(actor, "idle9");
        else
            return false;
    }

    void AiWander::setReturnPosition(const Ogre::Vector3& position)
    {
        if (!mHasReturnPosition)
        {
            mHasReturnPosition = true;
            mReturnPosition = position;
        }
    }

    void AiWander::getRandomIdle(short unsigned& playedIdle)
    {
        unsigned short idleRoll = 0;

        for(unsigned int counter = 0; counter < mIdle.size(); counter++)
        {
            static float fIdleChanceMultiplier = MWBase::Environment::get().getWorld()->getStore()
                .get<ESM::GameSetting>().find("fIdleChanceMultiplier")->getFloat();

            unsigned short idleChance = fIdleChanceMultiplier * mIdle[counter];
            unsigned short randSelect = (int)(rand() / ((double)RAND_MAX + 1) * int(100 / fIdleChanceMultiplier));
            if(randSelect < idleChance && randSelect > idleRoll)
            {
                playedIdle = counter+2;
                idleRoll = randSelect;
            }
        }
    }

    void AiWander::writeState(ESM::AiSequence::AiSequence &sequence) const
    {
        std::auto_ptr<ESM::AiSequence::AiWander> wander(new ESM::AiSequence::AiWander());
        wander->mData.mDistance = mDistance;
        wander->mData.mDuration = mDuration;
        wander->mData.mTimeOfDay = mTimeOfDay;
        wander->mStartTime = mStartTime.toEsm();
        assert (mIdle.size() == 8);
        for (int i=0; i<8; ++i)
            wander->mData.mIdle[i] = mIdle[i];
        wander->mData.mShouldRepeat = mRepeat;

        ESM::AiSequence::AiPackageContainer package;
        package.mType = ESM::AiSequence::Ai_Wander;
        package.mPackage = wander.release();
        sequence.mPackages.push_back(package);
    }

    AiWander::AiWander (const ESM::AiSequence::AiWander* wander)
        : mDistance(wander->mData.mDistance)
        , mDuration(wander->mData.mDuration)
        , mStartTime(MWWorld::TimeStamp(wander->mStartTime))
        , mTimeOfDay(wander->mData.mTimeOfDay)
        , mRepeat(wander->mData.mShouldRepeat)
    {
        for (int i=0; i<8; ++i)
            mIdle.push_back(wander->mData.mIdle[i]);

        init();
    }
}

