/*
 * examples/hopfield/HopfieldServer.cc
 *
 * Copyright (C) 2008 by Singularity Institute for Artificial Intelligence
 * Written by Joel Pitt <joel@fruitionnz.com>
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License v3 as
 * published by the Free Software Foundation and including the exceptions
 * at http://opencog.org/wiki/Licenses
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program; if not, write to:
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "HopfieldServer.h"

#include <sstream>
#include <iomanip>
#include <float.h>

#include <math.h>
#ifdef WIN32
#include <winsock2.h>
#else
#include <sys/time.h>
#endif
#include <unistd.h>

//#include <opencog/atomspace/utils.h>
#include <opencog/atomspace/Link.h>
#include <opencog/dynamics/attention/ImportanceUpdatingAgent.h>
#include <opencog/util/Logger.h>
#include <opencog/util/platform.h>
#include <opencog/util/mt19937ar.h>

#include "HopfieldUbigrapher.h"
#include "HopfieldOptions.h"
#include "StorkeyAgent.h"
#include "ImprintAgent.h"

extern "C" {
    #include <UbigraphAPI.h>
}

//! Inserts sleep statements so that visualisation in Ubigraph is clearer 
#define ENABLE_HOPFIELD_DELAYS 1

using namespace opencog;
using namespace std;

// factory method
BaseServer* HopfieldServer::derivedCreateInstance()
{
    logger().debug("[HopfieldServer] createInstance");
    return new HopfieldServer();
}

float HopfieldServer::totalEnergy()
{
    //! @warning this return erroneous results... needs to be fixed
    int N = hGrid.size();
    float E = 0.0f;
    AtomSpace *a = getAtomSpace();
    // sum for i<j
    for (int j = 1; j < N; j++) {
        for (int i = 0; i < N; i++) {
            if (i==j) continue;
            HandleSeq outgoing;
            outgoing.push_back(hGrid[i]);
            outgoing.push_back(hGrid[j]);
            
            HandleSeq ret;
            a->getHandleSet(back_inserter(ret), outgoing, NULL,
                    NULL, 2, HEBBIAN_LINK, true);
            // If no links then skip
            if (ret.size() == 0) { continue; }
            if (ret.size() > 1) {
                logger().error("More than one Hebbian link from unit i_%d to j_%d "
                        "while trying to get calculate energy of network.", i, j);
                cout << "ret size=" << ret.size() << " [0]=" << ret[0] << " [1]=" << ret[1] <<endl;
                return NAN;
            }
            float iSTI, jSTI;
            iSTI = a->getNormalisedSTI(hGrid[i],false);
            jSTI = a->getNormalisedSTI(hGrid[j],false);
            if (iSTI > 0.0f || jSTI > 0.0f) {
                switch (a->getType(ret[0])) {
                case SYMMETRIC_HEBBIAN_LINK:
                    if (iSTI > jSTI)
                        E += a->getTV(ret[0]).getMean() * (iSTI - jSTI);
                    else
                        E += a->getTV(ret[0]).getMean() * (jSTI - iSTI);
                    break;
                case INVERSE_HEBBIAN_LINK:
                    if (iSTI > 0.0f && iSTI > jSTI)
                        E += (a->getTV(ret[0]).getMean()) * (iSTI - jSTI);
                    break;
                case SYMMETRIC_INVERSE_HEBBIAN_LINK:
                    if (iSTI > jSTI)
                        E += (a->getTV(ret[0]).getMean()) * fabs(iSTI - jSTI);
                    else
                        E += (a->getTV(ret[0]).getMean()) * fabs(jSTI - iSTI);
                    break;
                default:
                    logger().error("Unknown Hebbian link type between unit s_%d and j_%d."
                            " Ignoring.", i, j);
                    break;
                }
            }
        }
    }
    E = E * -0.5f;
    float thresholdSum = 0.0f;
    for (int i = 0; i < N; i++) {
        thresholdSum += options->vizThreshold * a->getSTI(hGrid[i]);
    }
    E += thresholdSum;
    return E;
}

std::vector<float> HopfieldServer::imprintAndTestPattern(Pattern p, int imprint, int retrieve, Pattern cue, float mutate = 0.0f)
{
    std::vector<float> result;
    std::vector<int> rPattern;

    for (int i = 0; i < imprint; i++) {

        float iResult;
        if (options->cueGenerateOnce) {
            iResult = singleImprintAndTestPattern(p, retrieve, mutate, cue);
        } else {
            cue = p.mutatePattern(mutate);
            iResult = singleImprintAndTestPattern(p, retrieve, mutate, cue);
        }

        result.push_back(iResult);
        if (!options->verboseLevel) cout << "." << flush;
        if (i < imprint - 1 && options->recordToFile) {
            options->beforeFile << ", ";
            options->afterFile << ", ";
            options->diffFile << ", ";
        }

    }

    return result;

}

float HopfieldServer::singleImprintAndTestPattern(Pattern p, int retrieve = 1, float mutate = 0.0f, Pattern c = Pattern(0, 0))
{
    float result;
    float before = 0.0;
    Pattern rPattern(width, height);

    imprintPattern(p, 1);
    logger().fine("Encoded pattern for 1 loop");

//    if (options->visualize && ENABLE_HOPFIELD_DELAYS) sleep(5);

    if (! options->cueGenerateOnce) {
        c = p.mutatePattern(mutate);
        logger().fine("Mutated pattern");
    }

    if (options->recordToFile) {
        before = p.hammingSimilarity(c);
        options->beforeFile << before;
    }

    rPattern = retrievePattern(c, retrieve, options->spreadCycles, p);
    result = p.hammingSimilarity(rPattern);
    if (options->recordToFile) {
        options->afterFile << result;
        options->diffFile << (result - before);
    }
    if (options->visualize && ENABLE_HOPFIELD_DELAYS) sleep(5);

    // Nodes are left with STI after retrieval
    resetNodes();

    logger().fine("Retrieved pattern");

    return result;

}

template <class T>
inline string to_string (const T& t)
{
    stringstream ss;
    ss << t;
    return ss.str();
}

HopfieldServer::HopfieldServer()
{
    struct timeval tv;
    struct timezone tz;
    struct tm *tm;
    gettimeofday(&tv, &tz);
    time_t t = tv.tv_sec;
    tm = localtime(&t);

	patternStimulus = HDEMO_DEFAULT_PATTERN_STIM;
    width = HDEMO_DEFAULT_WIDTH;
    height = HDEMO_DEFAULT_HEIGHT;
    links = HDEMO_DEFAULT_LINKS;
    density = -1.0f;
    rng = new MT19937RandGen(tv.tv_usec);
    options = new HopfieldOptions();
    options->setServer(this);
    hebUpdateAgent = NULL;
    storkeyAgent = NULL;
    ubi = NULL;
}

HopfieldServer::~HopfieldServer()
{
    unloadModule("libattention.so");
    //delete importUpdateAgent;
    //delete hebUpdateAgent;
    delete rng;
}

void HopfieldServer::init(int width, int height, int numLinks)
{
    loadModule("libattention.so");

    //CogServer& cogserver = static_cast<CogServer&>(server());
    importUpdateAgent = static_cast<ImportanceUpdatingAgent*>(
            this->createAgent(ImportanceUpdatingAgent::info().id, true));
    if (options->updateMethod == HopfieldOptions::CONJUNCTION) {
        hebUpdateAgent = static_cast<HebbianUpdatingAgent*>(
                this->createAgent(HebbianUpdatingAgent::info().id, true));
    } else {
        storkeyAgent = new StorkeyAgent();
    }
    imprintAgent = new ImprintAgent();
    startAgent(imprintAgent);
    diffuseAgent = static_cast<ImportanceDiffusionAgent*>(
            this->createAgent(ImportanceDiffusionAgent::info().id, true));
    diffuseAgent->setSpreadDecider(ImportanceDiffusionAgent::HYPERBOLIC);
//    spreadAgent       = static_cast<ImportanceSpreadingAgent*>(this->createAgent(ImportanceSpreadingAgent::info().id, true));
    forgetAgent = static_cast<ForgettingAgent*>(
            this->createAgent(ForgettingAgent::info().id, true));

    if (options->verboseLevel) {
        importUpdateAgent->getLogger()->enable();
        importUpdateAgent->getLogger()->setPrintToStdoutFlag (true);
        if (hebUpdateAgent) {
            hebUpdateAgent->getLogger()->enable();
            hebUpdateAgent->getLogger()->setPrintToStdoutFlag (true);
        } else {
            storkeyAgent->getLogger()->enable();
            storkeyAgent->getLogger()->setPrintToStdoutFlag (true);
        }
//! @todo make all attention modules use their own logger object or upgrade
//! logging system to allow hierarchical logs.
#if 0
        diffuseAgent->getLogger()->enable();
        diffuseAgent->getLogger()->setPrintToStdoutFlag (true);
//        spreadAgent->getLogger()->enable();
//        spreadAgent->getLogger()->setPrintToStdoutFlag (true);

#endif
        imprintAgent->getLogger()->enable();
        imprintAgent->getLogger()->setPrintToStdoutFlag (true);
        forgetAgent->getLogger()->enable();
        forgetAgent->getLogger()->setPrintToStdoutFlag (true);
    }
    switch (options->verboseLevel) {
    case 1:
        importUpdateAgent->getLogger()->setLevel (Logger::INFO);
        forgetAgent->getLogger()->setLevel (Logger::INFO);
        if (hebUpdateAgent) hebUpdateAgent->getLogger()->setLevel (Logger::INFO);
        else storkeyAgent->getLogger()->setLevel (Logger::INFO);
        imprintAgent->getLogger()->setLevel (Logger::INFO);
        break;
    case 2:
        importUpdateAgent->getLogger()->setLevel (Logger::DEBUG);
        forgetAgent->getLogger()->setLevel (Logger::DEBUG);
        if (hebUpdateAgent) hebUpdateAgent->getLogger()->setLevel (Logger::DEBUG);
        else storkeyAgent->getLogger()->setLevel (Logger::DEBUG);
        imprintAgent->getLogger()->setLevel (Logger::INFO);
        break;
    case 3:
        importUpdateAgent->getLogger()->setLevel (Logger::FINE);
        forgetAgent->getLogger()->setLevel (Logger::FINE);
        if (hebUpdateAgent) hebUpdateAgent->getLogger()->setLevel (Logger::FINE);
        else storkeyAgent->getLogger()->setLevel (Logger::FINE);
        imprintAgent->getLogger()->setLevel (Logger::INFO);
        break;
    default:
        importUpdateAgent->getLogger()->setLevel (Logger::WARN);
        forgetAgent->getLogger()->setLevel (Logger::WARN);
        if (hebUpdateAgent) hebUpdateAgent->getLogger()->setLevel (Logger::WARN);
        else storkeyAgent->getLogger()->setLevel (Logger::WARN);
        imprintAgent->getLogger()->setLevel (Logger::INFO);
    }

    if (hebUpdateAgent)
        hebUpdateAgent->convertLinks = true;
    forgetAgent->forgetPercentage = options->forgetPercent;

    AtomSpace* atomSpace = getAtomSpace();

    string nodeName = "Hopfield_";
    if (width > 0) {
        this->width = width;
    }
    if (height > 0) {
        this->height = height;
    }

    if (numLinks > 0)
        this->links = numLinks;
    else if (density != -1.0f) {
        int maxLinks = 0, n;
        n = this->width * this->height - 1;
        maxLinks = n * (n + 1) / 2;
        this->links = (int) (density * maxLinks);
        logger().info("Density of %.2f gives %d links out of %d", density, this->links, maxLinks);
    }

    // Tune mind agents from command line options
    //spreadAgent->setSpreadThreshold(options->spreadThreshold);
    //spreadAgent->setImportanceSpreadingMultiplier(options->importanceSpreadingMultiplier);


    if (options->visualize) 
        ubi = new HopfieldUbigrapher();
    
    // Create nodes
    for (int i = 0; i < this->width; i++) {
        for (int j = 0; j < this->height; j++) {
            nodeName = "Hopfield_";
            nodeName += to_string(i) + "_" + to_string(j);
            Handle h = atomSpace->addNode(CONCEPT_NODE, nodeName.c_str());
            // We don't want the forgetting process to remove
            // the atoms perceiving the patterns
            atomSpace->setVLTI(h, AttentionValue::NONDISPOSABLE);
            hGrid.push_back(h);
            if (options->keyNodes) {
                hGridKey.push_back(false);
            }
        }
    }
    if (options->visualize)
        ubi->setGroundNode(hGrid[0]);

    // If only 1 node, don't try and connect it
    if (hGrid.size() < 2) {
        logger().warn("Only %d node(s), <2 so this is kind of silly but I shall follow instructions.", hGrid.size());
        return;
    }

    if (options->keyNodes)
        chooseKeyNodes();
    addRandomLinks();

	// make sure nodes are slightly negative if necessary
	// otherwise Hebbian Learning doesn't detect (get target conjunction == 0)
	resetNodes();

}

void HopfieldServer::chooseKeyNodes()
{
    uint currentKeyNodes = 0;
    if (options->keyNodes >= hGrid.size()) {
        logger().error("More keyNodes than perception atoms, not creating keyNodes");
        return;
    }

    while (currentKeyNodes < options->keyNodes) {
        int index = rng->randint(hGrid.size());
        if (!hGridKey[index]) {
            hGridKey[index] = true;
            currentKeyNodes++;
            keyNodes.push_back(hGrid[index]);
            if (options->visualize) ubi->setAsKeyNode(hGrid[index]);
        }
    }


}

void HopfieldServer::reset()
{
    AtomSpace* atomSpace = getAtomSpace();
    HandleEntry *links, *l;

    // Remove all links and replace
    links = atomSpace->getAtomTable().getHandleSet(HEBBIAN_LINK, true);
    for (l = links; l->next; l = l->next)
        atomSpace->removeAtom(l->handle);
    delete links;
    resetNodes();

    addRandomLinks();
}

void HopfieldServer::addRandomLinks()
{
    AtomSpace* atomSpace = getAtomSpace();
    HandleEntry *links;
    int amount, attempts = 0;
    int maxAttempts = 10000;

    // Add links if less than desired number and to replace forgotten links
    links = atomSpace->getAtomTable().getHandleSet(HEBBIAN_LINK, true);
    amount = this->links - links->getSize();
    delete links;

    logger().fine("Adding %d random Hebbian Links.", amount);
    // Link nodes randomly with amount links
    while (amount > 0 && attempts < maxAttempts) {
        int source, target;
        HandleSeq outgoing;
        HandleEntry* he;

        source = rng->randint(hGrid.size());
        target = rng->randint(hGrid.size() - 1);
        if (target >= source) target++;

        outgoing.push_back(hGrid[source]);
        outgoing.push_back(hGrid[target]);
        he = atomSpace->getAtomTable().getHandleSet(outgoing, (Type*) NULL, (bool*) NULL, outgoing.size(), HEBBIAN_LINK, true);
        // try links going the other way (because some Hebbian links are
        // ordered)
        if (!he) {
            outgoing.clear();
            outgoing.push_back(hGrid[target]);
            outgoing.push_back(hGrid[source]);
            he = atomSpace->getAtomTable().getHandleSet(outgoing, (Type*) NULL, (bool*) NULL, outgoing.size(), HEBBIAN_LINK, true);

        }
        if (he) {
            //logger().fine("Trying to add %d -> %d, but already exists %s", source, target, TLB::getAtom(he->handle)->toString().c_str());
            delete he;
            attempts++;
        } else {
            Handle rl = atomSpace->addLink(SYMMETRIC_HEBBIAN_LINK, outgoing);
            recentlyAddedLinks.push_back(rl);
            if (options->visualize) {
                ubi->setAsNewRandomLink(rl);
            }
            amount--;
            attempts = 0;
        }
    }


}

void HopfieldServer::resetNodes(bool toDefault)
{
    AtomSpace* a = getAtomSpace();
    HandleEntry *nodes, *n;

    nodes = a->getAtomTable().getHandleSet(NODE, true);

	if (toDefault) {
		for (n = nodes; n; n = n->next) {
			// Set all nodes to default STI and default LTI
			a->setSTI(n->handle, AttentionValue::DEFAULTATOMSTI);
			a->setLTI(n->handle, AttentionValue::DEFAULTATOMLTI);
		}
	} else {
		// Set nodes to negative of AF boundary - patternStimulus*wages
		AttentionValue::sti_t startSTI;
		AttentionValue::lti_t startLTI;
		startSTI = getAtomSpace()->getAttentionalFocusBoundary() -
			(patternStimulus * importUpdateAgent->getSTIAtomWage())/hGrid.size();
		startLTI = getAtomSpace()->getAttentionalFocusBoundary() -
			(patternStimulus * importUpdateAgent->getLTIAtomWage())/hGrid.size();
		for (n = nodes; n; n = n->next) {
			a->setSTI(n->handle, startSTI);
			a->setLTI(n->handle, startLTI);
		}
	}
    if (options->visualize) {
        ubi->applyStyleToType(CONCEPT_NODE, ubi->notPatternStyle);
        ubi->applyStyleToHandleSeq(keyNodes, ubi->keyNodeStyle);
    }
    
    delete nodes;

    logger().debug("Nodes Reset");
}

void HopfieldServer::updateKeyNodeLinks(Handle keyHandle, float density)
{
    AtomSpace *a = getAtomSpace();
    //! @todo: add links to only the active nodes within the pattern
    //! @todo: add density % links from the key node
    HandleSeq tempGrid(hGrid);

    // get all links from key node
    //std::map<Handle,Handle> mapDestToLink = getDestinationsFrom(keyHandle, HEBBIAN_LINK);
    HandleEntry* heNeighbours = TLB::getAtom(keyHandle)->getNeighbors(true, true, HEBBIAN_LINK);
    HandleSeq neighbours;
    heNeighbours->toHandleVector(neighbours);
    
    // for each entry in hGrid
    for (uint i = 0; i < hGrid.size(); i++) {
        // check that the position isn't a keyNode
        if (hGridKey[i]) continue;

        // check whether destination exists in the map
        if (find(neighbours.begin(), neighbours.end(), hGrid[i]) == neighbours.end()) {
            // it doesn't, so add it.
            a->addLink(SYMMETRIC_HEBBIAN_LINK, keyHandle, hGrid[i]);
        }
    
    }
    // randomly remove other links from other key nodes if # links > max
    HandleEntry* heLinks = atomSpace->getAtomTable().getHandleSet(HEBBIAN_LINK, true);
    HandleSeq links;
    heLinks->toHandleVector(links);
    delete heLinks;
    int amountToRemove = links.size() - this->links;
    if (amountToRemove > 0 && keyNodes.size() == 1) {
        logger().info("Only one keyNode, so unable to remove any links to "
                "compensate for the extra %d currently present.\n", amountToRemove);
        return;
    }
    if (amountToRemove > 0) {
        // construct a list of keyLinks eligible for removal
        HandleSeq eligibleForRemoval;
        // loop through keyNodes
        for (HandleSeq::iterator i = keyNodes.begin();
            i != keyNodes.end(); i++) {
            if (*i == keyHandle) continue;
            HandleSeq out = a->getOutgoing(*i);
            eligibleForRemoval.insert(eligibleForRemoval.end(),
                    out.begin(), out.end());
        }
        // concatenate all outgoing sets
        while (eligibleForRemoval.size() > 0 &&
                amountToRemove > 0) {
            // select random link
            int index = rng->randint(eligibleForRemoval.size());
            Handle lh = eligibleForRemoval[index];
            if (a->removeAtom(lh))
                amountToRemove--;
            else
                logger().error("Failed to remove link %s\n", TLB::getAtom(lh)->toString().c_str());
        }
    }
}

std::map<Handle,Handle> HopfieldServer::getDestinationsFrom(Handle src, Type linkType)
{
    //! This only expects arity 2 links, so make generic before placing in
    //! AtomSpace, by having the dest map keys be of type HandleSeq.
    //! returns in destinations mapped to link that got there.
    std::map<Handle,Handle> result;
    HandleSeq links = getAtomSpace()->getIncoming(src);
    HandleSeq::iterator j;
    for(j = links.begin(); j != links.end(); j++) {
        Handle lh = *j;
        if (!ClassServer::isAssignableFrom(linkType,getAtomSpace()->getType(lh)))
            continue;
        Handle destH;
        HandleSeq lseq = getAtomSpace()->getOutgoing(lh);
        // get handle at other end of the link
        for (HandleSeq::iterator k=lseq.begin();
                k < lseq.end() && destH == Handle::UNDEFINED; k++) {
            if (*k != src) {
                destH = *k; 
            }
        }
        result[destH] = lh;
    }
    return result;

}

Handle HopfieldServer::findKeyNode() {
    Handle keyHandle;
    AtomSpace *a = getAtomSpace();
    if (options->updateMethod == HopfieldOptions::CONJUNCTION) {
        // This could probably be done by diffusing the important, but the simpler
        // and hopefully quicker way is just to sum the weights * stimulus
        //
        // Also, there is a formula for selection in the glocal paper...
        
        // find closest matching key node (or unused key node)
        HandleSeq::iterator i;
        float maxSim = -1;
        for(i = keyNodes.begin(); i != keyNodes.end(); i++) {
            float sim = 0.0f;
            Handle iHandle = *i;
            //get all Hebbian links from keyHandle
            HandleSeq links = a->getIncoming(iHandle);
            HandleSeq::iterator j;
            for(j = links.begin(); j != links.end(); j++) {
                Handle lh = *j;
                Handle patternH;
                Type lt = a->getType(lh); 
                HandleSeq lseq = a->getOutgoing(lh);
                // get handle at other end of the link
                // TODO: create AtomSpace utility method that returns a map
                // between link and destination, see getDestinationsFrom
                for (HandleSeq::iterator k=lseq.begin();
                        k < lseq.end() && patternH == Handle::UNDEFINED; k++) {
                    if (*k != iHandle) {
                        patternH = *k; 
                    }
                }
                // check type of link 
                switch (lt) {
                case SYMMETRIC_HEBBIAN_LINK:
                    sim += a->getTV(lh).getMean() * a->getNormalisedSTI(patternH,false);
                    break;
                case ASYMMETRIC_HEBBIAN_LINK:
                    logger().error("Asymmetic links are not supported by the Hopfield "
                            "example, ignoring.");
                    break;
                case INVERSE_HEBBIAN_LINK:
                case SYMMETRIC_INVERSE_HEBBIAN_LINK:
                    sim += a->getTV(lh).getMean() * -a->getNormalisedSTI(patternH,false);
                    break;
                }
            }
            //cout << sim << endl;;
            if (sim > maxSim) {
                keyHandle = iHandle;
                maxSim = sim;
            }
            
        }
    } else {
        float minDiff = FLT_MAX;
        int keyIndex = -1;
        StorkeyAgent::w_t w(StorkeyAgent::getCurrentWeights());
        // Use inaccuracy method from glocal paper
        for(uint i = 0; i < keyNodes.size(); i++) {
            float diff = 0.0f;
            for (uint j = 0; j < hGrid.size(); j++) {
                if (hGridKey[j]) continue;
                
                diff += fabs(StorkeyAgent::h(i,j,w) * a->getNormalisedSTI(hGrid[j],false)) +
                    fabs(StorkeyAgent::h(j,i,w) * a->getNormalisedSTI(hGrid[i],false));
            }
            if (diff < minDiff) {
                minDiff = diff;
                keyIndex = i;
            }
        }
        if (keyIndex > 0)
            keyHandle = keyNodes[keyIndex];
        else
            keyHandle = keyNodes[0];
    }
    return keyHandle;

}

void HopfieldServer::imprintPattern(Pattern pattern, int cycles)
{
    static bool first = true;

    logger().fine("---Imprint:Begin");
    // loop for number of imprinting cyles
    for (int currCycles = 0; currCycles < cycles; currCycles++) {
        Handle keyNodeHandle;

        if (options->visualize) {
            ostringstream o;
            o << "Imprinting pattern: cycle " << currCycles;
            ubi->setText(o.str());
        }

        // encode pattern
        logger().fine("---Imprint:Encoding pattern with ImprintAgent");
        encodePattern(pattern, patternStimulus);

        printStatus();

        // ImportanceUpdating with links
        logger().fine("---Imprint:Running Importance update");
        importUpdateAgent->run(this);
        printStatus();

        if (options->visualize) {
            //unsigned char startRGB[3] = { 50, 50, 50 };
            //unsigned char endRGB[3] = { 100, 255, 80 };
            ubi->applyStyleToTypeGreaterThan(CONCEPT_NODE, ubi->patternStyle, Ubigrapher::STI, 0.5);
            /*for (int j=0; j < hGrid.size(); j++) {
                if (pattern[j]) 
                    ubi->updateSizeOfHandle(hGrid[j], Ubigrapher::STI, 2.0);
            }*/
        }

        // If using glocal key nodes, find the closest matching
        if (options->keyNodes) {
            logger().fine("---Imprint:Finding key node");
            keyNodeHandle = findKeyNode();
            // Make key node "Active"
            getAtomSpace()->setSTI(keyNodeHandle, (AttentionValue::sti_t)
                    patternStimulus / max(pattern.activity(),1));
            if (options->visualize) {
                // Set active key node style
                ubigraph_change_vertex_style(keyNodeHandle.value(), ubi->activeKeyNodeStyle);
            }
        }

        if (first)
            first = false;
        else
            logger().fine("---Imprint:Forgetting", totalEnergy());
        forgetAgent->run(this);

        if (options->keyNodes) {
            // add links from keyNode to pattern nodes
            logger().fine("---Imprint:Refreshing key node links");
            updateKeyNodeLinks(keyNodeHandle);
        }
        // avoid calculating energy if it won't be displayed
        if (logger().getLevel() == Logger::FINE) {
            logger().fine("---Imprint:Energy %f.", totalEnergy());
        }

        // add random links... only added if total links have not
        // passed maximum from the potential addition of keyNode links
        logger().fine("---Imprint:Adding random links");
        addRandomLinks();

        // then update with learning
        if (hebUpdateAgent) {
            logger().fine("---Imprint:Hebbian learning");
            hebUpdateAgent->run(this);
        } else {
            logger().fine("---Imprint:Storkey update rule");
            storkeyAgent->run(this);
        }
        if (options->visualize) {
            ubi->applyStyleToHandleSeq(recentlyAddedLinks,
                    ubi->compactLinkStyle);
            unsigned char startRGB2[3] = { 10, 10, 40 };
            unsigned char endRGB2[3] = { 80, 100, 255 };
            ubi->updateColourOfType(SYMMETRIC_INVERSE_HEBBIAN_LINK,
                    Ubigrapher::NONE, startRGB2, endRGB2);
            ubi->updateSizeOfType(HEBBIAN_LINK, Ubigrapher::TV_STRENGTH, 50.0, 0.1);
        }
        recentlyAddedLinks.clear();
// Unnecessary
//        logger().fine("---Imprint:Importance spreading");
//        diffuseAgent->run(this);
////        spreadAgent->run(this);

        printStatus();

        //logger().fine("---Imprint:Energy.");
        if (options->visualize && ENABLE_HOPFIELD_DELAYS) sleep(2);
        logger().fine("---Imprint:Resetting nodes");
        resetNodes();
    }
    logger().fine("---Imprint:End");

}

void HopfieldServer::encodePattern(Pattern pattern, stim_t stimulus)
{
	//int activity;
	
	// Avoid floating point exception if blank pattern
	//activity = pattern.activity();
	//if (activity == 0)
    //    activity = 1;
	//stim_t perUnit = stimulus / activity;

    // x2 because all nodes are -ve to begin with
//	stim_t perUnit = 2 * stimulus / hGrid.size();

//    for (size_t i = 0; i < hGrid.size(); i++) {
//        if (options->keyNodes && hGridKey[i]) continue; // Don't encode onto key nodes
//        getAtomSpace()->stimulateAtom(hGrid[i], perUnit * pattern[i]);
//    }
    getAtomSpace()->setSTI(imprintAgent, patternStimulus);
    imprintAgent->setPattern(pattern);
    imprintAgent->run(this);
}

std::vector<bool> HopfieldServer::checkNeighbourStability(Pattern p, float tolerance)
{
    std::vector<bool> stability;
    for (uint i = 0; i < p.size(); i++ ) {
        Pattern neighbour(p);
        neighbour[i]? neighbour[i] = 0: neighbour[i] = 1;
        Pattern result = retrievePattern(neighbour, options->retrieveCycles,
                options->spreadCycles, p);
        stability.push_back(p.hammingSimilarity(result) > (1.0f - tolerance));
    }
    return stability;

}

Pattern HopfieldServer::retrievePattern(Pattern partialPattern, int numCycles,
        int spreadCycles, Pattern originalPattern)
{
    std::string logString;
    int i = 0;

    logString += "---Retrieve:Initialising " + to_string(numCycles) + " cycle pattern retrieval process.";
    logger().info(logString.c_str());

    logger().fine("---Retrieve:Resetting nodes");
    resetNodes();

    if (options->visualize) {
        ostringstream o;
        o << "Retrieving pattern for " << numCycles << " cycles";
        ubi->setText(o.str());

    }
    while (i < numCycles) {
        logger().fine("---Retrieve:Encoding pattern");
        if (options->visualize) {
            ostringstream o;
            o << "Retrieving pattern: Cycle " << i;
            ubi->setText(o.str());
        }
        encodePattern(partialPattern, patternStimulus);
        printStatus();

        updateAtomTableForRetrieval(spreadCycles, originalPattern);
        printStatus();

        i++;
        logger().info("---Retreive:Cycles left %d", numCycles - i);
    }

    logString = "Cue pattern: \n" + patternToString(partialPattern);
    logger().info(logString.c_str());

    logger().info("---Retrieve:End");

    Pattern ret = getGridSTIAsPattern().binarisePattern(options->vizThreshold);
    if (options->keyNodes)
        ret.setMask(hGridKey);
    return ret;
}

Pattern HopfieldServer::getGridSTIAsPattern()
{
    Pattern out(width, height);
    std::vector<Handle>::iterator i;

    for (size_t i = 0; i < hGrid.size(); i++) {
        Handle h = hGrid[i];
        if (options->keyNodes && hGridKey[i]) {
            // Keynodes should be blank
            out[i] = 0;
        } else {
            out[i] = getAtomSpace()->getSTI(h);
        }
    }
    return out;
}

std::vector<stim_t> HopfieldServer::getGridStimVector()
{
    std::vector<stim_t> out;
    std::vector<Handle>::iterator i;

    for (i = hGrid.begin(); i != hGrid.end(); i++) {
        Handle h = *i;
        stim_t val;
        val = imprintAgent->getAtomStimulus(h); // / getAtomSpace()->getRecentMaxSTI();
        out.push_back( val );
    }

    return out;
}

void HopfieldServer::updateAtomTableForRetrieval(int spreadCycles = 1,
        Pattern originalPattern)
{
//   run ImportanceUpdatingAgent once without updating links

    bool oldLinksFlag = importUpdateAgent->getUpdateLinksFlag();
    importUpdateAgent->setUpdateLinksFlag(false);

    logger().info("---Retreive:Running Importance updating agent");
    importUpdateAgent->run(this);
    if (options->visualize) {
        if (originalPattern.size() == hGrid.size()) {
            // *** show agreement in green
            // *** show missing in blue if original passed
            // *** show extra in red if original passed
            ubi->showDiff(hGrid, getGridSTIAsPattern().binarisePattern(
                        options->vizThreshold), originalPattern);
        }
        ubi->updateSizeOfType(CONCEPT_NODE, Ubigrapher::STI, 3.0);
        if (ENABLE_HOPFIELD_DELAYS) sleep(10);
    }

    logger().info("---Retreive:Spreading Importance %d times", spreadCycles);
    //float temp = 1.0;
    //-- 
    //diffuseAgent->diffuseTemperature = 1.0f;
    //--
    float temp = 30.0;
    for (int i = 0; i < spreadCycles; i++) {
        logger().fine("---Retreive:Spreading Importance - cycle %d", i);
// Experimenting with some form of self-annealing...
//        diffuseAgent->setMaxSpreadPercentage(temp);
//        cout << "set max spread \% to " << temp << endl;
//--        
//        diffuseAgent->diffuseTemperature *= (spreadCycles - i)/( (float) spreadCycles + 1 );
//--
        diffuseAgent->setSpreadDecider(ImportanceDiffusionAgent::HYPERBOLIC,temp);
        temp *= 1.5;
        diffuseAgent->run(this);
        if (options->visualize) {
            if (originalPattern.size() == hGrid.size()) {
                ubi->showDiff(hGrid, getGridSTIAsPattern().binarisePattern(
                            options->vizThreshold), originalPattern);
            }
            ubi->updateSizeOfType(CONCEPT_NODE, Ubigrapher::STI, 3.0);
            if (ENABLE_HOPFIELD_DELAYS) sleep(2);
        }
//        temp *= (spreadCycles - i)/( (float) spreadCycles + 1 );
// Old spread agent.
//        spreadAgent->run(this);
    }

    importUpdateAgent->setUpdateLinksFlag(oldLinksFlag);

}

void HopfieldServer::printStatus()
{
    // Print out Node STIs in grid pattern.
    // Also print out binarised version of grid.
    Pattern nodeSTI = getGridSTIAsPattern();
    Pattern pattern = nodeSTI.binarisePattern(options->vizThreshold);
    std::vector<stim_t> nodeStim = getGridStimVector();
//    HandleEntry *links, *current_l;

    int i;
	

    int col;
    if (!options->verboseLevel) return;

    for (i = 0; i < height; i++) {
        for (col = 0; col < width; col++) {
            printf("% 1.2f ", nodeStim[i*width + col] / (float) patternStimulus);
        }
        cout << "| ";
        for (col = 0; col < width; col++) {
            printf("% 1.2f ", nodeSTI[i*width + col] / (float) getAtomSpace()->getMaxSTI());
        }
        cout << "| ";

        for (col = 0; col < width; col++) {
            printf("% 2d", pattern[i*width + col]);
        }
        cout << endl;
    }

    // Print out links.
//  if (options->verboseLevel > 1) {
// links = getAtomSpace()->getAtomTable().getHandleSet(HEBBIAN_LINK, true);
//
// for (current_l = links; current_l; current_l = current_l->next) {
//     Handle h = current_l->handle;
//
//     cout << TLB::getAtom(h)->toString() << endl;
//
// }
//  }
    cout << "-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=" << endl;
}

std::string HopfieldServer::printMatrixResult(std::vector< Pattern > patterns)
{
    int i, col;

    for (i = 0; i < height; i++) {
        for (unsigned int j = 0; j < patterns.size(); j++) {
            Pattern current = patterns[j];
            for (col = 0; col < width; col++) {
                int index=i*width + col;
                if (current.isMasked(index))
                    printf("  ");
                else if (j == (patterns.size()-1) &&
                        current[index] != patterns[0][index])
                    printf(" X");
                else 
                    printf("%2d", current[i*width + col]);
            }
            if (j != (patterns.size() - 1)) cout << " | ";
        }
        cout << endl;
    }
    return std::string();

}

void HopfieldServer::printLinks()
{
    HandleSeq hs;
    std::back_insert_iterator< HandleSeq > out_hi(hs);

    // Get all atoms (and subtypes) of type t
    getAtomSpace()->getHandleSet(out_hi, LINK, true);
    // For each, get prop, scale... and 
//    foreach (Handle h, hs) {
//        cout << TLB::getAtom(h)->toString() << endl;
//    }

}
