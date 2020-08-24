/**********************************************************************

  Audacity: A Digital Audio Editor

  ImportAUP.cpp

*//****************************************************************//**

\class AUPmportFileHandle
\brief An ImportFileHandle for AUP files (pre-AUP3)

*//****************************************************************//**

\class AUPImportPlugin
\brief An ImportPlugin for AUP files (pre-AUP3)

*//*******************************************************************/

#include "../Audacity.h" // for USE_* macros

#include "Import.h"
#include "ImportPlugin.h"

#include "../Envelope.h"
#include "../FileFormats.h"
#include "../FileNames.h"
#include "../LabelTrack.h"
#if defined(USE_MIDI)
#include "../NoteTrack.h"
#endif
#include "../Prefs.h"
#include "../Project.h"
#include "../ProjectFileIO.h"
#include "../ProjectFileIORegistry.h"
#include "../ProjectFileManager.h"
#include "../ProjectHistory.h"
#include "../ProjectSelectionManager.h"
#include "../ProjectSettings.h"
#include "../Tags.h"
#include "../TimeTrack.h"
#include "../ViewInfo.h"
#include "../WaveClip.h"
#include "../WaveTrack.h"
#include "../toolbars/SelectionBar.h"
#include "../widgets/AudacityMessageBox.h"
#include "../widgets/NumericTextCtrl.h"
#include "../widgets/ProgressDialog.h"
#include "../xml/XMLFileReader.h"

#include <map>

#define DESC XO("AUP project files (*.aup)")

static const auto exts = {wxT("aup")};

#include <wx/dir.h>
#include <wx/ffile.h>
#include <wx/file.h>
#include <wx/frame.h>
#include <wx/string.h>
#include <wx/utils.h>

class AUPImportFileHandle;
using ImportHandle = std::unique_ptr<ImportFileHandle>;

using NewChannelGroup = std::vector<std::shared_ptr<WaveTrack>>;

class AUPImportPlugin final : public ImportPlugin
{
public:
   AUPImportPlugin();
   ~AUPImportPlugin();

   wxString GetPluginStringID() override;
   
   TranslatableString GetPluginFormatDescription() override;

   ImportHandle Open(const FilePath &fileName,
                     AudacityProject *project) override;
};

class AUPImportFileHandle final : public ImportFileHandle,
                                  public XMLTagHandler
{
public:
   AUPImportFileHandle(const FilePath &name,
                       AudacityProject *project);
   ~AUPImportFileHandle();

   TranslatableString GetFileDescription() override;

   ByteCount GetFileUncompressedBytes() override;

   ProgressResult Import(WaveTrackFactory *trackFactory,
                         TrackHolders &outTracks,
                         Tags *tags) override;

   wxInt32 GetStreamCount() override;

   const TranslatableStrings &GetStreamInfo() override;

   void SetStreamUsage(wxInt32 WXUNUSED(StreamID), bool WXUNUSED(Use)) override;

   bool Open();

private:
   struct node
   {
      wxString parent;
      wxString tag;
      XMLTagHandler *handler;
   };
   using stack = std::vector<struct node>;

   bool HandleXMLTag(const wxChar *tag, const wxChar **attrs) override;
   void HandleXMLEndTag(const wxChar *tag) override;
   XMLTagHandler *HandleXMLChild(const wxChar *tag) override;

   bool HandleProject(XMLTagHandler *&handle);
   bool HandleLabelTrack(XMLTagHandler *&handle);
   bool HandleNoteTrack(XMLTagHandler *&handle);
   bool HandleTimeTrack(XMLTagHandler *&handle);
   bool HandleWaveTrack(XMLTagHandler *&handle);
   bool HandleTags(XMLTagHandler *&handle);
   bool HandleTag(XMLTagHandler *&handle);
   bool HandleLabel(XMLTagHandler *&handle);
   bool HandleWaveClip(XMLTagHandler *&handle);
   bool HandleSequence(XMLTagHandler *&handle);
   bool HandleWaveBlock(XMLTagHandler *&handle);
   bool HandleEnvelope(XMLTagHandler *&handle);
   bool HandleControlPoint(XMLTagHandler *&handle);
   bool HandleSimpleBlockFile(XMLTagHandler *&handle);
   bool HandleSilentBlockFile(XMLTagHandler *&handle);
   bool HandlePCMAliasBlockFile(XMLTagHandler *&handle);

   void AddFile(sampleCount len,
                const FilePath &filename = wxEmptyString,
                sampleCount origin = 0,
                int channel = 0);

   bool AddSilence(sampleCount len);
   bool AddSamples(const FilePath &filename,
                   sampleCount len,
                   sampleCount origin = 0,
                   int channel = 0);

   bool SetError(const TranslatableString &msg);
   bool SetWarning(const TranslatableString &msg);

private:
   AudacityProject &mProject;
   Tags *mTags;

   // project tag values that will be set in the actual project if the
   // import is successful
   #define field(n, t) bool have##n; t n
   struct
   {
      field(vpos, int);
      field(h, double);
      field(zoom, double);
      field(sel0, double);
      field(sel1, double);
#ifdef EXPERIMENTAL_SPECTRAL_EDITING
      field(selLow, double);
      field(selHigh, double);
#endif
      field(rate, double);
      field(snapto, bool);
      field(selectionformat, wxString);
      field(audiotimeformat, wxString);
      field(frequencyformat, wxString);
      field(bandwidthformat, wxString);
   } mProjectAttrs;
   #undef field

   typedef struct
   {
      WaveTrack *track;
      WaveClip *clip;
      FilePath path;
      sampleCount len;
      sampleCount origin;
      int channel;
   } fileinfo;
   std::vector<fileinfo> mFiles;
   sampleCount mTotalSamples;

   sampleFormat mFormat;
   unsigned long mSampleRate;
   unsigned long mNumChannels;

   stack mHandlers;
   wxString mParentTag;
   wxString mCurrentTag;
   const wxChar **mAttrs;

   wxFileName mProjDir;
   using BlockFileMap = std::map<wxString, FilePath>;
   BlockFileMap mFileMap;

   ListOfTracks mTracks;
   WaveTrack *mWaveTrack;
   WaveClip *mClip;

   ProgressResult mUpdateResult;
   TranslatableString mErrorMsg;
};

AUPImportPlugin::AUPImportPlugin()
:  ImportPlugin(FileExtensions(exts.begin(), exts.end()))
{
}

AUPImportPlugin::~AUPImportPlugin()
{
}

wxString AUPImportPlugin::GetPluginStringID()
{
   return wxT("legacyaup");
}
   
TranslatableString AUPImportPlugin::GetPluginFormatDescription()
{
   return DESC;
}

ImportHandle AUPImportPlugin::Open(const FilePath &fileName,
                                   AudacityProject *project)
{
   auto handle = std::make_unique<AUPImportFileHandle>(fileName, project);

   if (!handle->Open())
   {
      // Error or not something that we recognize
      return nullptr;
   }

   return handle;
}

static Importer::RegisteredImportPlugin registered
{
   "AUP", std::make_unique<AUPImportPlugin>()
};

AUPImportFileHandle::AUPImportFileHandle(const FilePath &fileName,
                                         AudacityProject *project)
:  ImportFileHandle(fileName),
   mProject(*project)
{
}

AUPImportFileHandle::~AUPImportFileHandle()
{
}

TranslatableString AUPImportFileHandle::GetFileDescription()
{
   return DESC;
}

auto AUPImportFileHandle::GetFileUncompressedBytes() -> ByteCount
{
   // TODO: Get Uncompressed byte count.
   return 0;
}

ProgressResult AUPImportFileHandle::Import(WaveTrackFactory *WXUNUSED(trackFactory),
                                           TrackHolders &WXUNUSED(outTracks),
                                           Tags *tags)
{
   auto &history = ProjectHistory::Get(mProject);
   auto &tracks = TrackList::Get(mProject);
   auto &viewInfo = ViewInfo::Get(mProject);
   auto &settings = ProjectSettings::Get(mProject);
   auto &selman = ProjectSelectionManager::Get(mProject);

   bool isDirty = history.GetDirty() || !tracks.empty();

   mTotalSamples = 0;

   mTags = tags;

   CreateProgress();

   mUpdateResult = ProgressResult::Success;

   XMLFileReader xmlFile;

   bool success = xmlFile.Parse(this, mFilename);
   if (!success)
   {
      mTracks.clear();

      AudacityMessageBox(
         XO("Couldn't import the project:\n\n%s").Format(xmlFile.GetErrorStr()),
         XO("Import Project"),
         wxOK | wxCENTRE,
         &GetProjectFrame(mProject));

      return ProgressResult::Failed;
   }

   if (!mErrorMsg.empty())
   {
      AudacityMessageBox(
         mErrorMsg,
         XO("Import Project"),
         wxOK | wxCENTRE,
         &GetProjectFrame(mProject));

      if (mUpdateResult == ProgressResult::Failed)
      {
         return ProgressResult::Failed;
      }
   }

   sampleCount processed = 0;
   for (auto fi : mFiles)
   {
      mUpdateResult = mProgress->Update(processed.as_long_long(), mTotalSamples.as_long_long());
      if (mUpdateResult != ProgressResult::Success)
      {
         return mUpdateResult;
      }

      mClip = fi.clip;
      mWaveTrack = fi.track;

      if (fi.path.empty())
      {
         AddSilence(fi.len);
      }
      else
      {
         AddSamples(fi.path, fi.len, fi.origin, fi.channel);
      }

      processed += fi.len;
   }
         
   if (mUpdateResult == ProgressResult::Failed || mUpdateResult == ProgressResult::Cancelled)
   {
      mTracks.clear();

      return mUpdateResult;
   }

   // Copy the tracks we just created into the project.
   for (auto &track : mTracks)
   {
      tracks.Add(track);
   }

   // Don't need our local track list anymore
   mTracks.clear();

   // If the active project is "dirty", then bypass the below updates as we don't
   // want to going changing things the user may have already set up.
   if (isDirty)
   {
      return mUpdateResult;
   }

   if (mProjectAttrs.haverate)
   {
      auto &bar = SelectionBar::Get(mProject);
      bar.SetRate(mProjectAttrs.rate);
   }

   if (mProjectAttrs.havesnapto)
   {
      selman.AS_SetSnapTo(mProjectAttrs.snapto ? SNAP_NEAREST : SNAP_OFF);
   }

   if (mProjectAttrs.haveselectionformat)
   {
      selman.AS_SetSelectionFormat(NumericConverter::LookupFormat(NumericConverter::TIME, mProjectAttrs.selectionformat));
   }

   if (mProjectAttrs.haveaudiotimeformat)
   {
      selman.TT_SetAudioTimeFormat(NumericConverter::LookupFormat(NumericConverter::TIME, mProjectAttrs.audiotimeformat));
   }

   if (mProjectAttrs.havefrequencyformat)
   {
      selman.SSBL_SetFrequencySelectionFormatName(NumericConverter::LookupFormat(NumericConverter::TIME, mProjectAttrs.frequencyformat));
   }

   if (mProjectAttrs.havebandwidthformat)
   {
      selman.SSBL_SetBandwidthSelectionFormatName(NumericConverter::LookupFormat(NumericConverter::TIME, mProjectAttrs.bandwidthformat));
   }

   // PRL: It seems this must happen after SetSnapTo
   if (mProjectAttrs.havevpos)
   {
      viewInfo.vpos = mProjectAttrs.vpos;
   }

   if (mProjectAttrs.haveh)
   {
      viewInfo.h = mProjectAttrs.h;
   }

   if (mProjectAttrs.havezoom)
   {
      viewInfo.SetZoom(mProjectAttrs.zoom);
   }

   if (mProjectAttrs.havesel0)
   {
      viewInfo.selectedRegion.setT0(mProjectAttrs.sel0);
   }

   if (mProjectAttrs.havesel1)
   {
      viewInfo.selectedRegion.setT1(mProjectAttrs.sel1);
   }

#ifdef EXPERIMENTAL_SPECTRAL_EDITING
   if (mProjectAttrs.haveselLow)
   {
      viewInfo.selectedRegion.setF0(mProjectAttrs.selLow);
   }

   if (mProjectAttrs.haveselHigh)
   {
      viewInfo.selectedRegion.setF1(mProjectAttrs.selHigh);
   }
#endif

   return mUpdateResult;
}

wxInt32 AUPImportFileHandle::GetStreamCount()
{
   return 1;
}

const TranslatableStrings &AUPImportFileHandle::GetStreamInfo()
{
   static TranslatableStrings empty;
   return empty;
}

void AUPImportFileHandle::SetStreamUsage(wxInt32 WXUNUSED(StreamID), bool WXUNUSED(Use))
{
}

bool AUPImportFileHandle::Open()
{
   wxFFile ff(mFilename, wxT("rb"));
   if (ff.IsOpened())
   {
      char buf[256];

      int numRead = ff.Read(buf, sizeof(buf));
      
      ff.Close();

      buf[sizeof(buf) - 1] = '\0';

      if (!wxStrncmp(buf, wxT("AudacityProject"), 15))
      {
         AudacityMessageBox(
            XO("This project was saved by Audacity version 1.0 or earlier. The format has\n"
               "changed and this version of Audacity is unable to import the project.\n\n"
               "Use a version of Audacity prior to v3.0.0 to upgrade the project and then\n"
               "you may import it with this version of Audacity."),
            XO("Import Project"),
            wxOK | wxCENTRE,
            &GetProjectFrame(mProject));

         return false;
      }

      if (wxStrncmp(buf, "<?xml", 5) == 0 &&
          (wxStrstr(buf, "<audacityproject") ||
           wxStrstr(buf, "<project") ))
      {
         return true;
      }
   }

   return false;
}

XMLTagHandler *AUPImportFileHandle::HandleXMLChild(const wxChar *tag)
{
   return this;
}

void AUPImportFileHandle::HandleXMLEndTag(const wxChar *tag)
{
   if (mUpdateResult != ProgressResult::Success)
   {
      return;
   }

   struct node node = mHandlers.back();

   if (wxStrcmp(tag, wxT("waveclip")) == 0)
   {
      mClip = static_cast<WaveClip *>(node.handler);
      mClip->HandleXMLEndTag(tag);
   }
   else
   {
      if (node.handler)
      {
         node.handler->HandleXMLEndTag(tag);
      }
   }

   mHandlers.pop_back();

   if (mHandlers.size())
   {
      node = mHandlers.back();
      mParentTag = node.parent;
      mCurrentTag = node.tag;
   }
}

bool AUPImportFileHandle::HandleXMLTag(const wxChar *tag, const wxChar **attrs)
{
   if (mUpdateResult != ProgressResult::Success)
   {
      return false;
   }

   mParentTag = mCurrentTag;
   mCurrentTag = tag;
   mAttrs = attrs;

   XMLTagHandler *handler = nullptr;
   bool success = false;

   if (mCurrentTag.IsSameAs(wxT("project")) ||
       mCurrentTag.IsSameAs(wxT("audacityproject")))
   {
      success = HandleProject(handler);
   }
   else if (mCurrentTag.IsSameAs(wxT("labeltrack")))
   {
      success = HandleLabelTrack(handler);
   }
   else if (mCurrentTag.IsSameAs(wxT("notetrack")))
   {
      success = HandleNoteTrack(handler);
   }
   else if (mCurrentTag.IsSameAs(wxT("timetrack")))
   {
      success = HandleTimeTrack(handler);
   }
   else if (mCurrentTag.IsSameAs(wxT("wavetrack")))
   {
      success = HandleWaveTrack(handler);
   }
   else if (mCurrentTag.IsSameAs(wxT("tags")))
   {
      success = HandleTags(handler);
   }
   else if (mCurrentTag.IsSameAs(wxT("tag")))
   {
      success = HandleTag(handler);
   }
   else if (mCurrentTag.IsSameAs(wxT("label")))
   {
      success = HandleLabel(handler);
   }
   else if (mCurrentTag.IsSameAs(wxT("waveclip")))
   {
      success = HandleWaveClip(handler);
   }
   else if (mCurrentTag.IsSameAs(wxT("sequence")))
   {
      success = HandleSequence(handler);
   }
   else if (mCurrentTag.IsSameAs(wxT("waveblock")))
   {
      success = HandleWaveBlock(handler);
   }
   else if (mCurrentTag.IsSameAs(wxT("envelope")))
   {
      success = HandleEnvelope(handler);
   }
   else if (mCurrentTag.IsSameAs(wxT("controlpoint")))
   {
      success = HandleControlPoint(handler);
   }
   else if (mCurrentTag.IsSameAs(wxT("simpleblockfile")))
   {
      success = HandleSimpleBlockFile(handler);
   }
   else if (mCurrentTag.IsSameAs(wxT("silentblockfile")))
   {
      success = HandleSilentBlockFile(handler);
   }
   else if (mCurrentTag.IsSameAs(wxT("pcmaliasblockfile")))
   {
      success = HandlePCMAliasBlockFile(handler);
   }

   if (!success || (handler && !handler->HandleXMLTag(tag, attrs)))
   {
      return SetError(XO("Internal error in importer...tag not recognized"));
   }

   mHandlers.push_back({mParentTag, mCurrentTag, handler});

   return true;
}

bool AUPImportFileHandle::HandleProject(XMLTagHandler *&handler)
{
   auto &fileMan = ProjectFileManager::Get(mProject);
   auto &window = GetProjectFrame(mProject);

   int requiredTags = 0;

   while (*mAttrs)
   {
      const wxChar *attr = *mAttrs++;
      const wxChar *value = *mAttrs++;
      long lValue;
      long long llValue;
      double dValue;

      if (!value)
      {
         break;
      }

      if (!XMLValueChecker::IsGoodString(value))
      {
         return SetError(XO("Invalid project '%s' attribute.").Format(attr));
      }

      wxString strValue = value;

#define set(f, v) (mProjectAttrs.have ## f = true, mProjectAttrs.f = v)

      // ViewInfo
      if (!wxStrcmp(attr, wxT("vpos")))
      {
         if (!XMLValueChecker::IsGoodInt(strValue) || !strValue.ToLong(&lValue) || (lValue < 0))
         {
            return SetError(XO("Invalid project 'vpos' attribute."));
         }

         set(vpos, (int) lValue);
      }
      else if (!wxStrcmp(attr, wxT("h")))
      {
         if (!Internat::CompatibleToDouble(value, &dValue) || (dValue < 0.0))
         {
            return SetError(XO("Invalid project 'h' attribute."));
         }

         set(h, dValue);
      }
      else if (!wxStrcmp(attr, wxT("zoom")))
      {
         if (!Internat::CompatibleToDouble(value, &dValue) || (dValue < 0.0))
         {
            return SetError(XO("Invalid project 'zoom' attribute."));
         }

         set(zoom, dValue);
      }
      // Viewinfo.SelectedRegion
      else if (!wxStrcmp(attr, wxT("sel0")))
      {
         if (!Internat::CompatibleToDouble(value, &dValue) || (dValue < 0.0))
         {
            return SetError(XO("Invalid project 'sel0' attribute."));
         }

         set(sel0, dValue);
      }
      else if (!wxStrcmp(attr, wxT("sel1")))
      {
         if (!Internat::CompatibleToDouble(value, &dValue) || (dValue < 0.0))
         {
            return SetError(XO("Invalid project 'sel1' attribute."));
         }

         set(sel1, dValue);
      }
#ifdef EXPERIMENTAL_SPECTRAL_EDITING
      else if (!wxStrcmp(attr, wxT("selLow")))
      {
         if (!Internat::CompatibleToDouble(value, &dValue) || (dValue < 0.0))
         {
            return SetError(XO("Invalid project 'selLow' attribute."));
         }

         set(selLow, dValue);
      }
      else if (!wxStrcmp(attr, wxT("selHigh")))
      {
         if (!Internat::CompatibleToDouble(value, &dValue) || (dValue < 0.0))
         {
            return SetError(XO("Invalid project 'selHigh' attribute."));
         }

         set(selHigh, dValue);
      }
#endif
      else if (!wxStrcmp(attr, wxT("version")))
      {
         requiredTags++;
      }

      else if (!wxStrcmp(attr, wxT("audacityversion")))
      {
         requiredTags++;
      }
      else if (!wxStrcmp(attr, wxT("projname")))
      {
         requiredTags++;

         mProjDir = mFilename;
         wxString altname = mProjDir.GetName() + wxT("-data");
         mProjDir.SetFullName(wxEmptyString);

         wxString projName = value;
         bool found = false;

         // First try to load the data files based on the _data dir given in the .aup file
         if (!projName.empty())
         {
            mProjDir.AppendDir(projName);
            if (!mProjDir.DirExists())
            {
               projName.clear();
            }
         }

         // If that fails then try to use the filename of the .aup as the base directory
         // This is because unzipped projects e.g. those that get transfered between mac-pc
         // may have encoding issues and end up expanding the wrong filenames for certain
         // international characters (such as capital 'A' with an umlaut.)
         if (projName.empty())
         {
            projName = altname;
            mProjDir.AppendDir(projName);
            if (!mProjDir.DirExists())
            {
               projName.clear();
            }
         }

         // No luck...complain and bail
         if (projName.empty())
         {
            AudacityMessageBox(
               XO("Couldn't find the project data folder: \"%s\"").Format(*value),
               XO("Error Opening Project"),
               wxOK | wxCENTRE,
               &window);

            return false;
         }

         // Collect and hash the file names within the project directory
         wxArrayString files;
         size_t cnt = wxDir::GetAllFiles(mProjDir.GetFullPath(),
                                         &files,
                                         "*.*");

         for (size_t i = 0; i < cnt; ++i)
         {
            FilePath fn = files[i];
            mFileMap[wxFileNameFromPath(fn)] = fn;
         }
      }
      else if (!wxStrcmp(attr, wxT("rate")))
      {
         if (!Internat::CompatibleToDouble(value, &dValue) || (dValue < 0.0))
         {
            return SetError(XO("Invalid project 'selLow' attribute."));
         }

         set(rate, dValue);
      }

      else if (!wxStrcmp(attr, wxT("snapto")))
      {
         set(snapto, (strValue == wxT("on") ? true : false));
      }

      else if (!wxStrcmp(attr, wxT("selectionformat")))
      {
         set(selectionformat, strValue);
      }

      else if (!wxStrcmp(attr, wxT("audiotimeformat")))
      {
         set(audiotimeformat, strValue);
      }

      else if (!wxStrcmp(attr, wxT("frequencyformat")))
      {
         set(frequencyformat, strValue);
      }

      else if (!wxStrcmp(attr, wxT("bandwidthformat")))
      {
         set(bandwidthformat, strValue);
      }
#undef set
   }

   if (requiredTags < 3)
   {
      return false;
   }

   // Do not set the handler - already handled

   return true;
}

bool AUPImportFileHandle::HandleLabelTrack(XMLTagHandler *&handler)
{
   mTracks.push_back(std::make_shared<LabelTrack>());

   handler = mTracks.back().get();

   return true;
}

bool AUPImportFileHandle::HandleNoteTrack(XMLTagHandler *&handler)
{
#if defined(USE_MIDI)
   mTracks.push_back( std::make_shared<NoteTrack>());

   handler = mTracks.back().get();

   return true;
#else
   AudacityMessageBox(
      XO("MIDI tracks found in project file, but this build of Audacity does not include MIDI support, bypassing track."),
      XO("Project Import"),
      wxOK | wxICON_EXCLAMATION | wxCENTRE,
      &GetProjectFrame(mProject));

   return false;
#endif
}

bool AUPImportFileHandle::HandleTimeTrack(XMLTagHandler *&handler)
{
   auto &tracks = TrackList::Get(mProject);

   // Bypass this timetrack if the project already has one
   // (See HandleTimeEnvelope and HandleControlPoint also)
   if (*tracks.Any<TimeTrack>().begin())
   {
      AudacityMessageBox(
         XO("The active project already has a time track and one was encountered in the project being imported, bypassing imported time track."),
         XO("Project Import"),
         wxOK | wxICON_EXCLAMATION | wxCENTRE,
         &GetProjectFrame(mProject));

      return true;
   }

   auto &viewInfo = ViewInfo::Get( mProject );
   mTracks.push_back( std::make_shared<TimeTrack>(&viewInfo) );

   handler = mTracks.back().get();

   return true;
}

bool AUPImportFileHandle::HandleWaveTrack(XMLTagHandler *&handler)
{
   auto &trackFactory = WaveTrackFactory::Get(mProject);
   mTracks.push_back(trackFactory.NewWaveTrack());

   handler = mTracks.back().get();

   mWaveTrack = static_cast<WaveTrack *>(handler);

   // No active clip.  In early versions of Audacity, there was a single
   // implied clip so we'll create a clip when the first "sequence" is
   // found.
   mClip = nullptr;

   return true;
}

bool AUPImportFileHandle::HandleTags(XMLTagHandler *&handler)
{
   wxString n;
   wxString v;

   // Support for legacy tags
   while(*mAttrs)
   {
      const wxChar *attr = *mAttrs++;
      const wxChar *value = *mAttrs++;

      if (!value)
      {
         break;
      }
      
      // Ignore empty tags
      if (!*value)
      {
         continue;
      }

      if (!XMLValueChecker::IsGoodString(attr) || !XMLValueChecker::IsGoodString(value))
      {
         // Log it???
         return false;
      }

      if (!wxStrcmp(attr, "id3v2"))
      {
         continue;
      }
      else if (!wxStrcmp(attr, "track"))
      {
         n = wxT("TRACKNUMBER");
      }
      else
      {
         n = attr;
         n.MakeUpper();
      }

      mTags->SetTag(n, value);
   }

   // Do not set the handler - already handled

   return true;
}

bool AUPImportFileHandle::HandleTag(XMLTagHandler *&handler)
{
   if (!mParentTag.IsSameAs(wxT("tags")))
   {
      return false;
   }

   wxString n, v;

   while (*mAttrs)
   {
      wxString attr = *mAttrs++;
      if (attr.empty())
      {
         break;
      }
      wxString value = *mAttrs++;

      if (!XMLValueChecker::IsGoodString(attr) || !XMLValueChecker::IsGoodString(value))
      {
         break;
      }

      if (attr == wxT("name"))
      {
         n = value;
      }
      else if (attr == wxT("value"))
      {
         v = value;
      }
   }

   if (n == wxT("id3v2"))
   {
      // LLL:  This is obsolete, but it must be handled and ignored.
   }
   else
   {
      mTags->SetTag(n, v);
   }

   // Do not set the handler - already handled

   return true;
}

bool AUPImportFileHandle::HandleLabel(XMLTagHandler *&handler)
{
   if (!mParentTag.IsSameAs(wxT("labeltrack")))
   {
      return false;
   }

   // The parent handler also handles this tag
   handler = mHandlers.back().handler;

   return true;
}

bool AUPImportFileHandle::HandleWaveClip(XMLTagHandler *&handler)
{
   struct node node = mHandlers.back();

   if (mParentTag.IsSameAs(wxT("wavetrack")))
   {
      WaveTrack *wavetrack = static_cast<WaveTrack *>(node.handler);

      handler = wavetrack->CreateClip();
   }
   else if (mParentTag.IsSameAs(wxT("waveclip")))
   {
      // Nested wave clips are cut lines
      WaveClip *waveclip = static_cast<WaveClip *>(node.handler);

      handler = waveclip->HandleXMLChild(mCurrentTag);
   }

   mClip = static_cast<WaveClip *>(handler);

   return true;
}

bool AUPImportFileHandle::HandleEnvelope(XMLTagHandler *&handler)
{
   struct node node = mHandlers.back();

   if (mParentTag.IsSameAs(wxT("timetrack")))
   {
      // If an imported timetrack was bypassed, then we want to bypass the
      // envelope as well.  (See HandleTimeTrack and HandleControlPoint)
      if (node.handler)
      {
         TimeTrack *timetrack = static_cast<TimeTrack *>(node.handler);

         handler = timetrack->GetEnvelope();
      }
   }
   // Earlier versions of Audacity had a single implied waveclip, so for
   // these versions, we get or create the only clip in the track.
   else if (mParentTag.IsSameAs(wxT("wavetrack")))
   {
      handler = mWaveTrack->RightmostOrNewClip()->GetEnvelope();
   }
   // Nested wave clips are cut lines
   else if (mParentTag.IsSameAs(wxT("waveclip")))
   {
      WaveClip *waveclip = static_cast<WaveClip *>(node.handler);

      handler = waveclip->GetEnvelope();
   }

   return true;
}

bool AUPImportFileHandle::HandleControlPoint(XMLTagHandler *&handler)
{
   struct node node = mHandlers.back();

   if (mParentTag.IsSameAs(wxT("envelope")))
   {
      // If an imported timetrack was bypassed, then we want to bypass the
      // control points as well.  (See HandleTimeTrack and HandleEnvelope)
      if (node.handler)
      {
         Envelope *envelope = static_cast<Envelope *>(node.handler);

         handler = envelope->HandleXMLChild(mCurrentTag);
      }
   }

   return true;
}

bool AUPImportFileHandle::HandleSequence(XMLTagHandler *&handler)
{
   struct node node = mHandlers.back();

   WaveClip *waveclip = static_cast<WaveClip *>(node.handler);

   while(*mAttrs)
   {
      const wxChar *attr = *mAttrs++;
      const wxChar *value = *mAttrs++;

      if (!value)
      {
         break;
      }

      long long nValue = 0;

      const wxString strValue = value;	// promote string, we need this for all

      if (!wxStrcmp(attr, wxT("maxsamples")))
      {
         // This attribute is a sample count, so can be 64bit
         if (!XMLValueChecker::IsGoodInt64(strValue) || !strValue.ToLongLong(&nValue) || (nValue < 0))
         {
            return SetError(XO("Invalid sequence 'maxsamples' attribute."));
         }

         // Dominic, 12/10/2006:
         //    Let's check that maxsamples is >= 1024 and <= 64 * 1024 * 1024
         //    - that's a pretty wide range of reasonable values.
         if ((nValue < 1024) || (nValue > 64 * 1024 * 1024))
         {
            return SetError(XO("Invalid sequence 'maxsamples' attribute."));
         }
      }
      else if (!wxStrcmp(attr, wxT("sampleformat")))
      {
         // This attribute is a sample format, normal int
         long fValue;
         if (!XMLValueChecker::IsGoodInt(strValue) || !strValue.ToLong(&fValue) || (fValue < 0) || !XMLValueChecker::IsValidSampleFormat(fValue))
         {
            return SetError(XO("Invalid sequence 'sampleformat' attribute."));
         }

         mFormat = (sampleFormat) fValue;
      }
      else if (!wxStrcmp(attr, wxT("numsamples")))
      {
         // This attribute is a sample count, so can be 64bit
         if (!XMLValueChecker::IsGoodInt64(strValue) || !strValue.ToLongLong(&nValue) || (nValue < 0))
         {
            return SetError(XO("Invalid sequence 'numsamples' attribute."));
         }
      }
   }

   // Do not set the handler - already handled

   return true;
}

bool AUPImportFileHandle::HandleWaveBlock(XMLTagHandler *&handler)
{
   while(*mAttrs)
   {
      const wxChar *attr = *mAttrs++;
      const wxChar *value = *mAttrs++;

      long long nValue = 0;

      if (!value)
      {
         break;
      }

      const wxString strValue = value;

      if (!wxStrcmp(attr, wxT("start")))
      {
         // making sure that values > 2^31 are OK because long clips will need them.
         if (!XMLValueChecker::IsGoodInt64(strValue) || !strValue.ToLongLong(&nValue) || (nValue < 0))
         {
            return SetError(XO("Unable to parse the waveblock 'start' attribute"));
         }
      }
   }

   // Do not set the handler - already handled

   return true;
}

bool AUPImportFileHandle::HandleSimpleBlockFile(XMLTagHandler *&handler)
{
   FilePath filename;
   sampleCount len = 0;
   
   while (*mAttrs)
   {
      const wxChar *attr =  *mAttrs++;
      const wxChar *value = *mAttrs++;
      long long nValue;

      if (!value)
      {
         break;
      }

      const wxString strValue = value;

      // Can't use XMLValueChecker::IsGoodFileName here, but do part of its test.
      if (!wxStricmp(attr, wxT("filename")))
      {
         if (XMLValueChecker::IsGoodFileString(strValue))
         {
            if (mFileMap.find(strValue) != mFileMap.end())
            {
               filename = mFileMap[strValue];
            }
            else
            {
               SetWarning(XO("Missing project file %s\n\nInserting silence instead.")
                  .Format(strValue));
            }
         }
      }
      else if (!wxStrcmp(attr, wxT("len")))
      {
         if (!XMLValueChecker::IsGoodInt64(strValue) || !strValue.ToLongLong(&nValue) || (nValue <= 0))
         {
            return SetError(XO("Missing or invalid simpleblockfile 'len' attribute."));
         }

         len = nValue;
      }
   }

   // Do not set the handler - already handled

   AddFile(len, filename);

   return true;
}

bool AUPImportFileHandle::HandleSilentBlockFile(XMLTagHandler *&handler)
{
   FilePath filename;
   sampleCount len = 0;
   
   while (*mAttrs)
   {
      const wxChar *attr =  *mAttrs++;
      const wxChar *value = *mAttrs++;
      long long nValue;

      if (!value)
      {
         break;
      }

      const wxString strValue = value;

      if (!wxStrcmp(attr, wxT("len")))
      {
         if (!XMLValueChecker::IsGoodInt64(value) || !strValue.ToLongLong(&nValue) || !(nValue > 0))
         {
            return SetError(XO("Missing or invalid silentblockfile 'len' attribute."));
         }

         len = nValue;
      }
   }

   // Do not set the handler - already handled

   AddFile(len);

   return true;
}

bool AUPImportFileHandle::HandlePCMAliasBlockFile(XMLTagHandler *&handler)
{
   wxFileName filename;
   sampleCount start = 0;
   sampleCount len = 0;
   int channel = 0;
   wxString name;

   while (*mAttrs)
   {
      const wxChar *attr =  *mAttrs++;
      const wxChar *value = *mAttrs++;
      long long nValue;

      if (!value)
      {
         break;
      }

      const wxString strValue = value;

      if (!wxStricmp(attr, wxT("aliasfile")))
      {
         if (XMLValueChecker::IsGoodPathName(strValue))
         {
            filename.Assign(strValue);
         }
         else if (XMLValueChecker::IsGoodFileName(strValue, mProjDir.GetPath()))
         {
            // Allow fallback of looking for the file name, located in the data directory.
            filename.Assign(mProjDir.GetPath(), strValue);
         }
         else if (XMLValueChecker::IsGoodPathString(strValue))
         {
            // If the aliased file is missing, we failed XMLValueChecker::IsGoodPathName()
            // and XMLValueChecker::IsGoodFileName, because both do existence tests.
            SetWarning(XO("Missing alias file %s\n\nInserting silence instead.")
               .Format(strValue));
         }
      }
      else if (!wxStricmp(attr, wxT("aliasstart")))
      {
         if (!XMLValueChecker::IsGoodInt64(strValue) || !strValue.ToLongLong(&nValue) || (nValue < 0))
         {
            return SetError(XO("Missing or invalid pcmaliasblockfile 'aliasstart' attribute."));
         }

         start = nValue;
      }
      else if (!wxStricmp(attr, wxT("aliaslen")))
      {
         if (!XMLValueChecker::IsGoodInt64(strValue) || !strValue.ToLongLong(&nValue) || (nValue <= 0))
         {
            return SetError(XO("Missing or invalid pcmaliasblockfile 'aliaslen' attribute."));
         }

         len = nValue;
      }
      else if (!wxStricmp(attr, wxT("aliaschannel")))
      {
         long nValue;
         if (!XMLValueChecker::IsGoodInt(strValue) || !strValue.ToLong(&nValue) || (nValue < 0))
         {
            return SetError(XO("Missing or invalid pcmaliasblockfile 'aliaslen' attribute."));
         }

         channel = nValue;
      }
   }

   // Do not set the handler - already handled

   AddFile(len, filename.GetFullPath(), start, channel);

   return true;

   if (!filename.IsOk())
   {
      return AddSilence(len);
   }

   return AddSamples(filename.GetFullPath(), len, start, channel);
}

void AUPImportFileHandle::AddFile(sampleCount len,
                                  const FilePath &filename /* = wxEmptyString */,
                                  sampleCount origin /* = 0 */,
                                  int channel /* = 0 */)
{
   fileinfo fi = {};
   fi.track = mWaveTrack;
   fi.clip = mClip;
   fi.path = filename;
   fi.len = len;
   fi.origin = origin,
   fi.channel = channel;

   mFiles.push_back(fi);

   mTotalSamples += len;
}

bool AUPImportFileHandle::AddSilence(sampleCount len)
{
   wxASSERT(mClip || mWaveTrack);

   if (mClip)
   {
      mClip->InsertSilence(mClip->GetEndTime(), mWaveTrack->LongSamplesToTime(len));
   }
   else if (mWaveTrack)
   {
      mWaveTrack->InsertSilence(mWaveTrack->GetEndTime(), mWaveTrack->LongSamplesToTime(len));
   }

   return true;
}

// All errors that occur here will simply insert silence and allow the
// import to continue.
bool AUPImportFileHandle::AddSamples(const FilePath &filename,
                                     sampleCount len,
                                     sampleCount origin /* = 0 */,
                                     int channel /* = 0 */)
{
   // Third party library has its own type alias, check it before
   // adding origin + size_t
   static_assert(sizeof(sampleCount::type) <= sizeof(sf_count_t),
                 "Type sf_count_t is too narrow to hold a sampleCount");

   SF_INFO info;
   memset(&info, 0, sizeof(info));

   wxFile f; // will be closed when it goes out of scope
   SNDFILE *sf = nullptr;
   bool success = false;

   auto cleanup = finally([&]
   {
      if (!success)
      {
         SetWarning(XO("Error while processing %s\n\nInserting silence.").Format(filename));

         AddSilence(len);
      }

      if (sf)
      {
         sf_close(sf);
      }
   });

   if (!f.Open(filename))
   {
      SetWarning(XO("Failed to open %s").Format(filename));

      return true;
   }

   // Even though there is an sf_open() that takes a filename, use the one that
   // takes a file descriptor since wxWidgets can open a file with a Unicode name and
   // libsndfile can't (under Windows).
   sf = sf_open_fd(f.fd(), SFM_READ, &info, FALSE);
   if (!sf)
   {
      SetWarning(XO("Failed to open %s").Format(filename));

      return true;
   }

   if (origin > 0)
   {
      if (sf_seek(sf, origin.as_long_long(), SEEK_SET) < 0)
      {
         SetWarning(XO("Failed to seek to position %lld in %s")
            .Format(origin.as_long_long(), filename));

         return true;
      }
   }

   sampleFormat format = mFormat;
   sf_count_t cnt = len.as_size_t();
   int channels = info.channels;

   wxASSERT(channels >= 1);
   wxASSERT(channel < channels);

   SampleBuffer buffer(cnt, format);
   samplePtr bufptr = buffer.ptr();

   size_t framesRead = 0;
   
   if (channels == 1 && format == int16Sample && sf_subtype_is_integer(info.format))
   {
      // If both the src and dest formats are integer formats,
      // read integers directly from the file, conversions not needed
      framesRead = sf_readf_short(sf, (short *) bufptr, cnt);
   }
   else if (channels == 1 && format == int24Sample && sf_subtype_is_integer(info.format))
   {
      framesRead = sf_readf_int(sf, (int *) bufptr, cnt);
      if (framesRead != cnt)
      {
         SetWarning(XO("Unable to read %lld samples from %s")
            .Format(cnt, filename));

         return true;
      }

      // libsndfile gave us the 3 byte sample in the 3 most
      // significant bytes -- we want it in the 3 least
      // significant bytes.
      int *intPtr = (int *) bufptr;
      for (size_t i = 0; i < framesRead; i++)
      {
         intPtr[i] = intPtr[i] >> 8;
      }
   }
   else if (format == int16Sample && !sf_subtype_more_than_16_bits(info.format))
   {
      // Special case: if the file is in 16-bit (or less) format,
      // and the calling method wants 16-bit data, go ahead and
      // read 16-bit data directly.  This is a pretty common
      // case, as most audio files are 16-bit.
      SampleBuffer temp(cnt * channels, int16Sample);
      short *tmpptr = (short *) temp.ptr();

      framesRead = sf_readf_short(sf, tmpptr, cnt);
      if (framesRead != cnt)
      {
         SetWarning(XO("Unable to read %lld samples from %s")
            .Format(cnt, filename));

         return true;
      }

      for (size_t i = 0; i < framesRead; i++)
      {
         ((short *)bufptr)[i] = tmpptr[(channels * i) + channel];
      }
   }
   else
   {
      // Otherwise, let libsndfile handle the conversion and
      // scaling, and pass us normalized data as floats.  We can
      // then convert to whatever format we want.
      SampleBuffer tmpbuf(cnt * channels, floatSample);
      float *tmpptr = (float *) tmpbuf.ptr();

      framesRead = sf_readf_float(sf, tmpptr, cnt);
      if (framesRead != cnt)
      {
         SetWarning(XO("Unable to read %lld samples from %s")
            .Format(cnt, filename));

         return true;
      }

      CopySamples((samplePtr)(tmpptr + channel),
                  floatSample,
                  bufptr,
                  format,
                  framesRead,
                  true /* high quality by default */,
                  channels /* source stride */);
   }

   wxASSERT(mClip || mWaveTrack);

   // Add the samples to the clip/track
   if (mClip)
   {
      mClip->Append(bufptr, format, cnt);
      mClip->Flush();
   }
   else if (mWaveTrack)
   {
      mWaveTrack->Append(bufptr, format, cnt);
      mWaveTrack->Flush();
   }

   // Let the finally block know everything is good
   success = true;

   return true;
}

bool AUPImportFileHandle::SetError(const TranslatableString &msg)
{
   wxLogError(msg.Translation());

   if (mErrorMsg.empty())
   {
      mErrorMsg = msg;
   }

   mUpdateResult = ProgressResult::Failed;

   return false;
}

bool AUPImportFileHandle::SetWarning(const TranslatableString &msg)
{
   wxLogWarning(msg.Translation());

   if (mErrorMsg.empty())
   {
      mErrorMsg = msg;
   }

   return false;
}
