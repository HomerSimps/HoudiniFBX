/*
 * Copyright (c) 2022
 *	Side Effects Software Inc.  All rights reserved.
 *
 * Redistribution and use of in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. The name of Side Effects Software may not be used to endorse or
 *    promote products derived from this software without specific prior
 *    written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY SIDE EFFECTS SOFTWARE `AS IS' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN
 * NO EVENT SHALL SIDE EFFECTS SOFTWARE BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * NAME:	ROP library (C++)
 *
 * COMMENTS:	Class for FBX output.
 *
 */

#include "ROP_FBXHeaderWrapper.h"
#include "ROP_FBXActionManager.h"
#include "ROP_FBXExporter.h"
#include "ROP_FBXAnimVisitor.h"
#include "ROP_FBXDerivedActions.h"
#include "ROP_FBXMainVisitor.h"
#include "ROP_FBXUtil.h"

#include <OBJ/OBJ_Node.h>
#include <SOP/SOP_Node.h>

#include <GU/GU_Detail.h>
#include <GU/GU_DetailHandle.h>
#include <GEO/GEO_Primitive.h>
#include <GEO/GEO_Vertex.h>

#include <FBX/FBX_AllocWrapper.h>

#include <MOT/MOT_Director.h>

#include <OP/OP_Bundle.h>
#include <OP/OP_BundleList.h>
#include <OP/OP_Director.h>
#include <OP/OP_Network.h>
#include <OP/OP_Node.h>
#include <OP/OP_Take.h>
#include <CH/CH_Manager.h>

#include <TAKE/TAKE_Manager.h>
#include <TAKE/TAKE_Take.h>

#include <UT/UT_Assert.h>
#include <UT/UT_Interrupt.h>
#include <UT/UT_ScopeExit.h>
#include <UT/UT_UndoManager.h>

#include <SYS/SYS_Version.h>

// Always declare these variables although they are only modified when
// compiling debug.
double ROP_FBXdb_maxVertsCountingTime;
double ROP_FBXdb_vcacheExportTime;
double ROP_FBXdb_cookingTime;
double ROP_FBXdb_convexTime;
double ROP_FBXdb_reorderTime;
double ROP_FBXdb_convertTime;
double ROP_FBXdb_duplicateTime;

using namespace std;

/********************************************************************************************************/
ROP_FBXExporter::ROP_FBXExporter()
{
    mySDKManager = NULL;
    myScene = NULL;
    myNodeManager = NULL;
    myActionManager = NULL;
    myDummyRootNullNode = NULL;
    myBoss = NULL;
    myDidCancel = false;
    myErrorManager = new ROP_FBXErrorManager();
}
/********************************************************************************************************/
ROP_FBXExporter::~ROP_FBXExporter()
{
    if(myErrorManager)
	delete myErrorManager;
    myErrorManager = NULL;
}
/********************************************************************************************************/
bool 
ROP_FBXExporter::initializeExport(const char* output_name, fpreal tstart, fpreal tend, ROP_FBXExportOptions* options)
{
    if(!output_name)
	return false;

    deallocateQueuedStrings();

#ifdef UT_DEBUG
    ROP_FBXdb_vcacheExportTime = 0;
    ROP_FBXdb_maxVertsCountingTime = 0;
    ROP_FBXdb_cookingTime = 0;
    ROP_FBXdb_convexTime = 0;
    ROP_FBXdb_reorderTime = 0;
    ROP_FBXdb_convertTime = 0;
    ROP_FBXdb_duplicateTime = 0;
    myDBStartTime = clock();
#endif
    myErrorManager->reset();

    myStartTime = tstart;
    myEndTime = tend;

    if(options)
	myExportOptions = *options;
    else
	myExportOptions.reset();

    myNodeManager = new ROP_FBXNodeManager;
    myActionManager = new ROP_FBXActionManager(*myNodeManager, *myErrorManager, *this);

    FBXwrapAllocators();

    // Initialize the fbx scene manager
    mySDKManager = FbxManager::Create();

    if (!mySDKManager)
    {
//	addError(ROP_COOK_ERROR, "Unable to create the FBX SDK manager");
	return false;
    }

    // Create the entity that will hold the scene.
    myScene = FbxScene::Create(mySDKManager,"");
    myOutputFile = output_name;

    myDidCancel = false;
    myDummyRootNullNode = NULL;

    return true;
}
/********************************************************************************************************/
void 
ROP_FBXExporter::doExport()
{
    UT_AutoDisableUndos disable_undos_scope;
    UT_AutoInterrupt progress("Exporting FBX");

    myBoss = progress.getInterrupt();
    UT_AT_SCOPE_EXIT(myBoss = nullptr);

    if (progress.wasInterrupted())
	return;

    // See if we're exporting bundles
    if(myExportOptions.isExportingBundles())
    {
	// Parse bundle names
	UT_String bundle_names(UT_String::ALWAYS_DEEP, myExportOptions.getBundlesString());

	// Strip any '@' we might have
	bundle_names.strip("@");

	OP_Node* bundle_node;
	OP_Bundle *bundle;
	OP_BundleList *bundles = OPgetDirector()->getBundles();
	int bundle_idx, node_idx;

	OP_Node* obj_net = OPgetDirector()->findNode("/obj");
	OP_Node* top_network = NULL;

	for (bundle_idx = 0; bundle_idx < bundles->entries(); bundle_idx++)
	{
	    bundle = bundles->getBundle(bundle_idx);
	    UT_ASSERT(bundle);
	    if (!bundle)
		continue;
	    UT_String bundle_name(bundle->getName());

	    if (!bundle_name.multiMatch(bundle_names))
		continue;

	    // Process the bundle
	    for (node_idx = 0; node_idx < bundle->entries(); node_idx++)
	    {
		bundle_node = bundle->getNode(node_idx);
		UT_ASSERT(bundle_node);
		if (!bundle_node)
		    continue;
		myNodeManager->addBundledNode(bundle_node);

		if(!top_network)
		{
		    top_network = bundle_node->getParent();
		    if(top_network->getIsContainedBy(obj_net) == false)
			top_network = NULL;
		}
		else
		{
		    // Find the parent network which contains all nodes so far.
		    while(top_network != obj_net && bundle_node->getIsContainedBy(top_network) == false)
			top_network = top_network->getParent();
		}
	    } // end over all nodes in a bundle
	} // end over all bundles

	if(!top_network)
	    top_network = obj_net;

	// Now set the top exported network
	UT_String start_path;
	top_network->getFullPath(start_path);
	myExportOptions.setStartNodePath(start_path, false);
    }

    // Set the exported take
    OP_Take	*take_mgr = OPgetDirector()->getTakeManager();
    TAKE_Take *init_take = take_mgr->getCurrentTake();

    // Find and set the needed take
    if(strlen(myExportOptions.getExportTakeName()) == 0)
    {
	// Export current take.
	init_take = NULL;
    }
    else
	take_mgr->takeSet(myExportOptions.getExportTakeName());
    
    // Restore original take on exit
    UT_SCOPE_EXIT
    {
	if(init_take)
	    take_mgr->takeSet(init_take->getName());
    };

    // Export geometry first
    ROP_FBXMainVisitor geom_visitor(this);

    FbxGlobalSettings& scene_settings = myScene->GetGlobalSettings();


    // Set Application Info 
    FbxDocumentInfo* scene_info = myScene->GetSceneInfo();
    scene_info->Original_ApplicationVendor.Set("SideFX Software");
    scene_info->Original_ApplicationName.Set("Houdini");
    scene_info->Original_ApplicationVersion.Set(SYS_Version::full());
    UT_String outfile = myOutputFile.c_str();
#if _WIN32
    outfile.substitute("/", "\\");
#endif
    scene_info->Original_FileName.Set(outfile.c_str());
    {
        // Add ApplicationActiveProject and ApplicationNativeFile properties
        // despite the fact that there's no native SDK support for them. Both
        // Maya and MotionBuilder seem to output them and Eidos Interactive
        // needed it (Bug #107732).
        CH_Manager *mgr = CHgetManager();
        fpreal t = CHgetEvalTime();
        UT_String job;
        mgr->expandString("$JOB", job, t);
        if (job.isstring())
        {
#if _WIN32
            job.substitute("/", "\\");
#endif
            FbxPropertyT<FbxString> prop = FbxProperty::Create(
                    scene_info->Original, FbxStringDT,
                    "ApplicationActiveProject");
            prop.Set(job.c_str());
        }
        UT_String hipfile;
        mgr->expandString("$HIPFILE", hipfile, t);
        if (hipfile.isstring())
        {
#if _WIN32
            hipfile.substitute("/", "\\");
#endif
            FbxPropertyT<FbxString> prop = FbxProperty::Create(
                    scene_info->Original, FbxStringDT, "ApplicationNativeFile");
            prop.Set(hipfile.c_str());
        }
    }

    scene_info->LastSaved_ApplicationVendor.Set("SideFX Software");
    scene_info->LastSaved_ApplicationName.Set("Houdini");
    scene_info->LastSaved_ApplicationVersion.Set(SYS_Version::full());

    auto now = FbxDateTime::currentDateTimeGMT();
    scene_info->Original_DateTime_GMT.Set(now);
    scene_info->LastSaved_DateTime_GMT.Set(now);


    bool exporting_single_frame = !getExportingAnimation();
    CH_Manager *ch_manager = CHgetManager();
    FbxTime fbx_start, fbx_stop;


    // If we're outputting a single frame, we don't allow exporting deforms as vertex caches
    if(exporting_single_frame)
    {
	// This is a copy of the originally passed-in options,
	// so it's ok to change it.
	myExportOptions.setExportDeformsAsVC(false);
    }

    if(!exporting_single_frame)
    {
        // NOTE: Using FbxTime::ConvertFrameRateToTimeMode() seems to not support everything here!
	fpreal curr_fps = ch_manager->getSamplesPerSec();
	FbxTime::EMode time_mode = FbxTime::eFrames24;
	if(SYSisEqual(curr_fps, 24.0))
	    time_mode = FbxTime::eFrames24;
	else if(SYSisEqual(curr_fps, 120.0))
	    time_mode = FbxTime::eFrames120;
	else if(SYSisEqual(curr_fps, 100.0))
	    time_mode = FbxTime::eFrames100;
	else if(SYSisEqual(curr_fps, 60.0))
	    time_mode = FbxTime::eFrames60;
	else if(SYSisEqual(curr_fps, 50.0))
	    time_mode = FbxTime::eFrames50;
	else if(SYSisEqual(curr_fps, 48.0))
	    time_mode = FbxTime::eFrames48;
	else if(SYSisEqual(curr_fps, 30.0))
	    time_mode = FbxTime::eFrames30;
	else if(SYSisEqual(curr_fps, 29.97))
	    time_mode = FbxTime::eNTSCFullFrame;
	else if(SYSisEqual(curr_fps, 25.0))
	    time_mode = FbxTime::ePAL;
	else if(SYSisEqual(curr_fps, 1000.0))
	    time_mode = FbxTime::eFrames1000;
	else if(SYSisEqual(curr_fps, 23.976))
	    time_mode = FbxTime::eFilmFullFrame;
	else if(SYSisEqual(curr_fps, 96.0))
	    time_mode = FbxTime::eFrames96;
	else if(SYSisEqual(curr_fps, 72.0))
	    time_mode = FbxTime::eFrames72;
	else if(SYSisEqual(curr_fps, 59.94))
	    time_mode = FbxTime::eFrames59dot94;
	else
            time_mode = FbxTime::eCustom;
	scene_settings.SetTimeMode(time_mode);
        scene_settings.SetCustomFrameRate(curr_fps);     // sets frame rate in the scene
	FbxTime::SetGlobalTimeMode(time_mode, curr_fps); // governs how time is converted

	fbx_start.SetFrame(CHgetFrameFromTime(myStartTime), time_mode);
	fbx_stop.SetFrame(CHgetFrameFromTime(myEndTime), time_mode);

	FbxTimeSpan time_span(fbx_start, fbx_stop);
	scene_settings.SetTimelineDefaultTimeSpan(time_span);

    }

    // Note: what about geom networks in other parts of the scene?
    OP_Node* geom_node;
    geom_node = OPgetDirector()->findNode(myExportOptions.getStartNodePath());

    if(!geom_node)
    {
	// Issue a warning and quit.
	myErrorManager->addError("Could not find the start node specified [ ",myExportOptions.getStartNodePath()," ]",true);
	return;
    }

    geom_visitor.visitScene(geom_node);
    myDidCancel = geom_visitor.getDidCancel();

    // Create any instances, if necessary
    if(geom_visitor.getCreateInstancesAction())
	geom_visitor.getCreateInstancesAction()->performAction();

    if(!myDidCancel)
    {
	// Global light settings - set the global ambient.
	FbxColor fbx_col;
	float amb_col[3];
	UT_Color amb_color = geom_visitor.getAccumAmbientColor();
	amb_color.getRGB(&amb_col[0], &amb_col[1], &amb_col[2]);
	fbx_col.Set(amb_col[0],amb_col[1],amb_col[2]);
	FbxGlobalLightSettings& global_light_settings = myScene->GlobalLightSettings();
	global_light_settings.SetAmbientColor(fbx_col);

	// Export animation if applicable
	if(!exporting_single_frame)
	{ 
	    ROP_FBXAnimVisitor anim_visitor(this);

	    TAKE_Take *curr_hd_take = OPgetDirector()->getTakeManager()->getCurrentTake();

	    FbxAnimLayer* anim_layer = FbxAnimLayer::Create(myScene, "Base Layer");

	    FbxTime::EMode time_mode = scene_settings.GetTimeMode();

	    int num_clips = myExportOptions.getNumExportClips();
	    if (num_clips > 0)
	    {
		for (int i = 0; i < num_clips; ++i)
		{
		    ROP_FBXExportClip anim_clip = myExportOptions.getExportClip(i);
		    FbxAnimStack* anim_stack = FbxAnimStack::Create(myScene, anim_clip.name);
		    anim_stack->AddMember(anim_layer);

		    FbxTime fbx_start, fbx_stop;

		    fbx_start.SetFrame((anim_clip.start_frame), time_mode);
		    fbx_stop.SetFrame((anim_clip.end_frame), time_mode);
		    FbxTimeSpan time_span(fbx_start, fbx_stop);
		    anim_stack->SetLocalTimeSpan(time_span);
		    anim_stack->SetReferenceTimeSpan(time_span);
		}

	    }
	    else 
	    {
		FbxAnimStack* anim_stack = FbxAnimStack::Create(myScene, curr_hd_take->getName());
		anim_stack->AddMember(anim_layer);
	    }



	    anim_visitor.reset(anim_layer);

	    // Create a single default animation stack.

	    // Export the main world_root animation if applicable
	    if(myDummyRootNullNode)
	    {			
		//FbxTakeNode* curr_world_take_node = ROP_FBXAnimVisitor::addFBXTakeNode(myDummyRootNullNode);
		anim_visitor.exportTRSAnimation(geom_node->castToOBJNode(), anim_layer, myDummyRootNullNode);
	    }	    

	    anim_visitor.visitScene(geom_node);
	    myDidCancel = anim_visitor.getDidCancel();

	}
	// Perform post-actions
	if(!myDidCancel)
	    myActionManager->performPostActions();

        // Setup the axis system into the file, converting if needed
        FbxAxisSystem scene_axis_system = scene_settings.GetAxisSystem();
        switch (OPgetDirector()->getOrientationMode())
        {
            case OP_OrientationMode::Y_UP:
                scene_axis_system = FbxAxisSystem::MayaYUp;
                break;
            case OP_OrientationMode::Z_UP:
                scene_axis_system = FbxAxisSystem::MayaZUp;
                break;
        }
        if (myExportOptions.getConvertAxisSystem()
            && myExportOptions.getAxisSystem() != ROP_FBXAxisSystem_Current)
        {
            FbxAxisSystem target_axis;
            switch (myExportOptions.getAxisSystem())
            {
                case ROP_FBXAxisSystem_YUp_RightHanded:
                    target_axis = FbxAxisSystem::MayaYUp;
                    UT_ASSERT(target_axis == FbxAxisSystem::Motionbuilder);
                    UT_ASSERT(target_axis == FbxAxisSystem::OpenGL);
                    break;
                case ROP_FBXAxisSystem_YUp_LeftHanded:
                    target_axis = FbxAxisSystem::DirectX;
                    UT_ASSERT(target_axis == FbxAxisSystem::Lightwave);
                    break;
                case ROP_FBXAxisSystem_ZUp_RightHanded:
                    target_axis = FbxAxisSystem::MayaZUp;
                    UT_ASSERT(target_axis == FbxAxisSystem::Max);
                    break;
                case ROP_FBXAxisSystem_Current:
                    UT_ASSERT(!"Should not get here");
                    target_axis = scene_axis_system;
                    break;
            }
            if (target_axis != scene_axis_system)
            {
                scene_settings.SetOriginalUpAxis(scene_axis_system);
                target_axis.ConvertScene(myScene);
                UT_ASSERT(scene_settings.GetAxisSystem() == target_axis);
            }
        }
        else // Set axis system without conversion
        {
            switch (myExportOptions.getAxisSystem())
            {
                case ROP_FBXAxisSystem_YUp_RightHanded:
                    scene_settings.SetAxisSystem(FbxAxisSystem::MayaYUp);
                    break;
                case ROP_FBXAxisSystem_YUp_LeftHanded:
                    scene_settings.SetAxisSystem(FbxAxisSystem::DirectX);
                    break;
                case ROP_FBXAxisSystem_ZUp_RightHanded:
                    scene_settings.SetAxisSystem(FbxAxisSystem::Max);
                    break;
                case ROP_FBXAxisSystem_Current:
                    scene_settings.SetAxisSystem(scene_axis_system);
                    break;
            }
        }

        if (myExportOptions.getConvertUnits())
        {
            // FbxSystemUnit takes scale factor as number of centimeters
            const int unit = myExportOptions.getconvertUnitTo();            
            FbxSystemUnit hou_units = (CHgetManager()->getUnitLength() * 100);        
            
            scene_settings.SetSystemUnit(hou_units);
            scene_settings.SetOriginalSystemUnit(hou_units);

            switch (unit)
            {
            case UNIT_MM:
                FbxSystemUnit::mm.ConvertScene(myScene);
                break;
            case UNIT_CM:
                FbxSystemUnit::cm.ConvertScene(myScene);
                break;
            case UNIT_DM:
                FbxSystemUnit::dm.ConvertScene(myScene);
                break;
            case UNIT_M:                    
                FbxSystemUnit::m.ConvertScene(myScene);
                break;
            case UNIT_KM:
                FbxSystemUnit::km.ConvertScene(myScene);
                break;            
            case UNIT_IN:
                FbxSystemUnit::Inch.ConvertScene(myScene);
                break;
            case UNIT_YA:
                FbxSystemUnit::Yard.ConvertScene(myScene);
                break;
            case UNIT_ML:
                FbxSystemUnit::Mile.ConvertScene(myScene);
                break;


            }

        }
    }
}
/********************************************************************************************************/
bool
ROP_FBXExporter::finishExport()
{
#ifdef UT_DEBUG
    double write_time_start, write_time_end;
    write_time_start = clock();
#endif

    bool bSuccess = false;
    if(!myDidCancel)
    {
	// Save the built-up scene
	FbxExporter* fbx_exporter = FbxExporter::Create(mySDKManager, "");

	string sdk_full_version = myExportOptions.getVersion();
	string sdk_exporter_name, sdk_version;
	int sep_pos = sdk_full_version.find('|');
	if(sep_pos > 0)
	{
	    sdk_exporter_name = sdk_full_version.substr(0, sep_pos - 1);
	    sdk_version = sdk_full_version.substr(sep_pos + 2);
	}

	if(sdk_exporter_name.length() <= 0)
	    sdk_exporter_name = "FBX";

	// Append ascii or binary string
	if(myExportOptions.getExportInAscii())
	    sdk_exporter_name += " ascii";
	else
	    sdk_exporter_name += " binary";

	int format_index, format_count = mySDKManager->GetIOPluginRegistry()->GetWriterFormatCount();
	int out_file_format = -1;

	for (format_index = 0; format_index < format_count; format_index++)
	{
	    if (mySDKManager->GetIOPluginRegistry()->WriterIsFBX(format_index))
	    {
		FbxString format_desc = mySDKManager->GetIOPluginRegistry()->GetWriterFormatDescription(format_index);
		if(format_desc.GetLen() >= sdk_exporter_name.length() && 
		    sdk_exporter_name == format_desc.Left(sdk_exporter_name.length()).Buffer())
		{
		    out_file_format = format_index;
		    break;
		}
	    }
	}

	// Deprecated.
	///fbx_exporter->SetFileFormat(out_file_format);

	if(sdk_version.length() > 0)
	    fbx_exporter->SetFileExportVersion(sdk_version.c_str(), FbxSceneRenamer::eFBX_TO_FBX);
#if 0	
	// Options are now done differenty. Luckily, we don't use them.
	FbxStreamOptionsFbxWriter* export_options = FbxStreamOptionsFbxWriter::Create(mySDKManager, "");
	if (mySDKManager->GetIOPluginRegistry()->WriterIsFBX(out_file_format))
	{
	    // Set the export states. By default, the export states are always set to 
	    // true except for the option eEXPORT_TEXTURE_AS_EMBEDDED. The code below 
	    // shows how to change these states.
	    /*
	    export_options->SetOption(KFBXSTREAMOPT_FBX_MATERIAL, true);
	    export_options->SetOption(KFBXSTREAMOPT_FBX_TEXTURE, true);
	    export_options->SetOption(KFBXSTREAMOPT_FBX_EMBEDDED, pEmbedMedia);
	    export_options->SetOption(KFBXSTREAMOPT_FBX_LINK, true);
	    export_options->SetOption(KFBXSTREAMOPT_FBX_SHAPE, true);
	    export_options->SetOption(KFBXSTREAMOPT_FBX_GOBO, true);
	    export_options->SetOption(KFBXSTREAMOPT_FBX_ANIMATION, true);
	    export_options->SetOption(KFBXSTREAMOPT_FBX_GLOBAL_SETTINGS, true); */
	}
#endif
	// Initialize the exporter by providing a filename.
	if(fbx_exporter->Initialize(myOutputFile.c_str(), out_file_format, mySDKManager->GetIOSettings()) == false)
	    return false;

	// Embed media if option is enabled via the UI
	FbxIOSettings* io_settings = fbx_exporter->GetIOSettings();
	io_settings->SetBoolProp(EXP_FBX_EMBEDDED, myExportOptions.getEmbedMedia());

	// Export the scene.
	bSuccess = fbx_exporter->Export(myScene);
	if (!bSuccess)
	{
	    UT_VERIFY(false);
	    // Issue a warning and quit.
	    myErrorManager->addError("FbxExporter::Initialize() failed. ", "Error returned: ", fbx_exporter->GetStatus().GetErrorString(), true);
	}	

	// Destroy the exporter.
	fbx_exporter->Destroy();
    }

    if(myScene)
	myScene->Destroy();
    myScene = NULL;

    if(mySDKManager)
	mySDKManager->Destroy();
    mySDKManager = NULL;

    deallocateQueuedStrings();

#ifdef UT_DEBUG
    write_time_end = clock();
#endif

    if(myNodeManager)
	delete myNodeManager;
    myNodeManager = NULL;

    if(myActionManager)
	delete myActionManager;
    myActionManager = NULL;


#ifdef UT_DEBUG
    myDBEndTime = clock();

    // Print results
    double total_time = myDBEndTime - myDBStartTime;
    double temp_time;

    printf("Max Vertex Count Time: %.2f secs ( %.2f%%) \n", ((double)ROP_FBXdb_maxVertsCountingTime) / ((double)CLOCKS_PER_SEC), ROP_FBXdb_maxVertsCountingTime / total_time * 100.0 );
    if (ROP_FBXdb_maxVertsCountingTime > 0)
    {
	printf("\tPure Cooking Time: %.2f secs ( %.2f%%) \n", ((double)ROP_FBXdb_cookingTime) / ((double)CLOCKS_PER_SEC), ROP_FBXdb_cookingTime / ROP_FBXdb_maxVertsCountingTime * 100.0 );        
	printf("\tDuplication Time: %.2f secs ( %.2f%%) \n", ((double)ROP_FBXdb_duplicateTime) / ((double)CLOCKS_PER_SEC), ROP_FBXdb_duplicateTime / ROP_FBXdb_maxVertsCountingTime * 100.0 );
	printf("\tConversion Time: %.2f secs ( %.2f%%) \n", ((double)ROP_FBXdb_convertTime) / ((double)CLOCKS_PER_SEC), ROP_FBXdb_convertTime / ROP_FBXdb_maxVertsCountingTime * 100.0 );
	printf("\tTri Time: %.2f secs ( %.2f%%) \n", ((double)ROP_FBXdb_convexTime) / ((double)CLOCKS_PER_SEC), ROP_FBXdb_convexTime / ROP_FBXdb_maxVertsCountingTime * 100.0 );
	printf("\tReordering Time: %.2f secs ( %.2f%%) \n", ((double)ROP_FBXdb_reorderTime) / ((double)CLOCKS_PER_SEC), ROP_FBXdb_reorderTime / ROP_FBXdb_maxVertsCountingTime * 100.0 );
    }
    printf("Vertex Caching Time: %.2f secs ( %.2f%%) \n", ((double)ROP_FBXdb_vcacheExportTime) / ((double)CLOCKS_PER_SEC), ROP_FBXdb_vcacheExportTime / total_time * 100.0 );
    temp_time = write_time_end - write_time_start;
    printf("File Write Time: %.2f secs ( %.2f%%) \n", ((double)temp_time) / ((double)CLOCKS_PER_SEC), temp_time / total_time * 100.0 );
    
    printf("Total Export Time: %.2f secs \n\n", ((double)total_time) / ((double)CLOCKS_PER_SEC) );
#endif

    return bSuccess;
}
/********************************************************************************************************/
FbxManager* 
ROP_FBXExporter::getSDKManager()
{
    return mySDKManager;
}
/********************************************************************************************************/
FbxScene* 
ROP_FBXExporter::getFBXScene()
{
    return myScene;
}
/********************************************************************************************************/
ROP_FBXErrorManager* 
ROP_FBXExporter::getErrorManager()
{
    return myErrorManager;
}
/********************************************************************************************************/
ROP_FBXNodeManager* 
ROP_FBXExporter::getNodeManager()
{
    return myNodeManager;
}
/********************************************************************************************************/
ROP_FBXActionManager* 
ROP_FBXExporter::getActionManager()
{
    return myActionManager;
}
/********************************************************************************************************/
ROP_FBXExportOptions* 
ROP_FBXExporter::getExportOptions()
{
    return &myExportOptions;
}
/********************************************************************************************************/
const char* 
ROP_FBXExporter::getOutputFileName()
{
    return myOutputFile.c_str();
}
/********************************************************************************************************/
fpreal 
ROP_FBXExporter::getStartTime()
{
    return myStartTime;
}
/********************************************************************************************************/
fpreal 
ROP_FBXExporter::getEndTime()
{
    return myEndTime;
}
/********************************************************************************************************/
bool 
ROP_FBXExporter::getExportingAnimation()
{
    return !SYSisEqual(myStartTime, myEndTime);
}
/********************************************************************************************************/
void 
ROP_FBXExporter::queueStringToDeallocate(char* string_ptr)
{
    myStringsToDeallocate.push_back(string_ptr);
}
/********************************************************************************************************/
void 
ROP_FBXExporter::deallocateQueuedStrings()
{
    int curr_str_idx, num_strings = myStringsToDeallocate.size();
    for(curr_str_idx = 0; curr_str_idx < num_strings; curr_str_idx++)
    {
	delete[] myStringsToDeallocate[curr_str_idx];
    }
    myStringsToDeallocate.clear();
}
/********************************************************************************************************/
FbxNode* 
ROP_FBXExporter::getFBXRootNode(OP_Node* asking_node, bool create_subnet_root)
{
    // If we are exporting one of the standard subnets, such as "/" or "/obj", etc., return the
    // fbx scene root. Otherwise, create a null node (if it's not created yet), and return that.

    FbxNode* fbx_scene_root = myScene->GetRootNode();

    UT_String export_path(myExportOptions.getStartNodePath());

    if(export_path == "/")
	return fbx_scene_root;

    // Try to get the parent network
    OP_Node* export_node = OPgetDirector()->findNode(export_path);
    if(!export_node)
	return fbx_scene_root;

    // If our parent network is the same as us, just return the fbx scene root (this happens
    // when exporting a single GEO node, for example, or in general when exporting a network
    // that is not to be dived into).
    if(asking_node == export_node)
	return fbx_scene_root;

    OP_Network* parent_net = export_node->getParentNetwork();
    if(!parent_net)
	return fbx_scene_root;

    parent_net->getFullPath(export_path);
    if(export_path == "/")
	return fbx_scene_root;

    if (!create_subnet_root)
	return fbx_scene_root;

    if(!myDummyRootNullNode)
    {
	UT_String node_name(UT_String::ALWAYS_DEEP, "world_root");
	myNodeManager->makeNameUnique(node_name);

	myDummyRootNullNode = FbxNode::Create(mySDKManager, (const char*)node_name);
	FbxNull *res_attr = FbxNull::Create(mySDKManager, (const char*)node_name);
	res_attr->Look.Set(FbxNull::eNone);
	myDummyRootNullNode->SetNodeAttribute(res_attr);

	// Set world transform
	ROP_FBXUtil::setStandardTransforms(export_node, myDummyRootNullNode, NULL, 0.0, getStartTime(), true);
	fbx_scene_root->AddChild(myDummyRootNullNode);

	// Add nodes to the map
	ROP_FBXMainNodeVisitInfo dummy_info(export_node);
	dummy_info.setFbxNode(myDummyRootNullNode);
	myNodeManager->addNodePair(export_node, myDummyRootNullNode, dummy_info);
    }

    UT_ASSERT(myDummyRootNullNode);
    return myDummyRootNullNode;
}
/********************************************************************************************************/
UT_Interrupt* 
ROP_FBXExporter::GetBoss()
{
    return myBoss;
}
/********************************************************************************************************/
void 
ROP_FBXExporter::getVersions(TStringVector& versions_out)
{
    versions_out.clear();

    FBXwrapAllocators();

    FbxManager* tempSDKManager = FbxManager::Create();
    if(!tempSDKManager)
	return;

    // Get versions for the first available format. This assumes that versions for the
    // ascii and binary exporters are the same. Which they are.
    int format_index, format_count = tempSDKManager->GetIOPluginRegistry()->GetWriterFormatCount();
    string str_temp;
    int curr_ver;
    int out_file_format = -1;

    for (format_index = 0; format_index < format_count; format_index++)
    {
	if (tempSDKManager->GetIOPluginRegistry()->WriterIsFBX(format_index))
	{
	    FbxString format_desc = tempSDKManager->GetIOPluginRegistry()->GetWriterFormatDescription(format_index);

	    // Skip any encrypted formats.
	    if (format_desc.Find("encrypted") >= 0)
		continue;

	    // Note that we don't want duplicate ascii and binary formats here, since that is filtered
	    // later when we actually do the export. So we skip the ascii version, and strip the binary
	    // word out of the description.
	    if (format_desc.Find("ascii") >= 0)
		continue;

	    // Note: This assumes the first FBX writer format we have find
	    // is the one we actually want to use.
	    if(out_file_format < 0)
		out_file_format = format_index;

	    // Get all versions of the current format.
	    int binary_pos;
	    char const* const* curr_format_versions = tempSDKManager->GetIOPluginRegistry()->GetWritableVersions(format_index);
	    if(curr_format_versions)
	    {
		for(curr_ver = 0; curr_format_versions[curr_ver]; curr_ver++)
		{
		    // We need to concatenate the format name and version, since now
		    // it's not just a single format with multiple versions, but multiple
		    // formats with multiple versions.
		    str_temp = format_desc.Buffer();
		    binary_pos = str_temp.find("binary");
		    UT_ASSERT(binary_pos != string::npos && binary_pos > 0);
		    // -1 to skip a space before the format.
		    str_temp = str_temp.substr(0, binary_pos - 1);
		    str_temp += " | ";
		    str_temp += curr_format_versions[curr_ver];
		    versions_out.push_back(str_temp);
		}
	    }
	}
    }

    tempSDKManager->Destroy();
}
/********************************************************************************************************/
